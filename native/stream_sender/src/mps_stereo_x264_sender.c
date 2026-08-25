/* Two independent H.264 (x264, software/NEON) eye streams, each its own UDP
 * port pair -- no SBS composition, no RGBA conversion, no IMT importance-map
 * weighting. That custom pixel/QP work existed to serve the combined-SBS
 * design; splitting the eyes removes the need for essentially all of it, so
 * this file is deliberately much smaller than mps_imt_rpicam_sbs_sender.c.
 *
 * The two heavy per-frame costs left (camera ISP capture, H.264 encode) are
 * both delegated to mature upstream libraries (rpicam-vid/libcamera, x264)
 * that already carry their own hand-written ARM NEON kernels -- there is no
 * meaningful custom-code SIMD opportunity left on top of that without
 * duplicating what those libraries already do.
 *
 * Wire layout: left eye on <port>, right eye on <port + 1>, each an
 * independent IMT byte stream (own frame_seq, own FEC groups, own
 * keyframe-per-frame policy from mps_x264_encoder). No cross-eye
 * synchronisation beyond what the shared camera-sync capture thread already
 * provides at capture time.
 */
#ifndef _WIN32
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#ifdef _WIN32
#error "mps_stereo_x264_sender targets Linux/Pi only."
#endif

#include "imt.h"
#include "mps_x264_encoder.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAPTURE_SLOT_COUNT 3
#define MPS_PACING_MIN_BYTES_PER_SEC 1000000ull
#define MPS_PACING_INITIAL_BYTES_PER_SEC 25000000ull
#define MPS_PACING_BATCH_DATAGRAMS 32u

#ifndef SO_MAX_PACING_RATE
#define SO_MAX_PACING_RATE 47
#endif

typedef struct {
    int fd;
    struct sockaddr_in addr;
    uint64_t bytes_this_second;
    uint64_t pacing_window_start_ns;
    uint64_t pacing_batch_bytes;
    uint32_t pacing_batch_datagrams;
    uint64_t target_rate_bytes_per_sec;
} SendCtx;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Self-adapting rate cap: after each 1s window, retarget SO_MAX_PACING_RATE
 * to 2x what was actually sent, so a quiet link doesn't stay throttled to
 * its startup guess and a busy one doesn't run unbounded. */
static int emit_datagram(const uint8_t* data, size_t len, void* user)
{
    SendCtx* ctx = (SendCtx*)user;
    uint64_t now;
    const ssize_t sent = sendto(ctx->fd, data, len, 0,
                                (const struct sockaddr*)&ctx->addr, sizeof(ctx->addr));
    if (sent != (ssize_t)len) {
        return -1;
    }

    now = now_ns();
    if (ctx->pacing_window_start_ns == 0u) {
        ctx->pacing_window_start_ns = now;
    }
    ctx->bytes_this_second += (uint64_t)len;
    if (now - ctx->pacing_window_start_ns >= 1000000000ull) {
        uint64_t next_rate = ctx->bytes_this_second * 2u;
        if (next_rate < MPS_PACING_MIN_BYTES_PER_SEC) {
            next_rate = MPS_PACING_MIN_BYTES_PER_SEC;
        }
        if (next_rate > 0xffffffffull) {
            next_rate = 0xffffffffull;
        }
        ctx->target_rate_bytes_per_sec = next_rate;
        {
            const unsigned int rate = (unsigned int)next_rate;
            (void)setsockopt(ctx->fd, SOL_SOCKET, SO_MAX_PACING_RATE, &rate, sizeof(rate));
        }
        ctx->bytes_this_second = 0u;
        ctx->pacing_window_start_ns = now;
    }

    ctx->pacing_batch_bytes += (uint64_t)len;
    ctx->pacing_batch_datagrams += 1u;
    if (ctx->pacing_batch_datagrams >= MPS_PACING_BATCH_DATAGRAMS &&
        ctx->target_rate_bytes_per_sec > 0u) {
        const uint64_t sleep_ns =
            (ctx->pacing_batch_bytes * 1000000000ull) / ctx->target_rate_bytes_per_sec;
        if (sleep_ns > 0u) {
            struct timespec delay;
            delay.tv_sec = (time_t)(sleep_ns / 1000000000ull);
            delay.tv_nsec = (long)(sleep_ns % 1000000000ull);
            while (clock_nanosleep(CLOCK_MONOTONIC, 0, &delay, &delay) != 0 && errno == EINTR) {
            }
        }
        ctx->pacing_batch_bytes = 0u;
        ctx->pacing_batch_datagrams = 0u;
    }
    return 0;
}

static int bind_udp_source_port(int fd, uint16_t port)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    return bind(fd, (const struct sockaddr*)&addr, sizeof(addr));
}

static void set_thread_affinity_mask(pthread_t thread, const int* cores, size_t count,
                                     const char* name)
{
    cpu_set_t cpuset;
    size_t i;
    int rc;
    if (!cores || count == 0u) {
        return;
    }
    CPU_ZERO(&cpuset);
    for (i = 0u; i < count; ++i) {
        CPU_SET(cores[i], &cpuset);
    }
    rc = pthread_setaffinity_np(thread, sizeof(cpuset), &cpuset);
    if (rc != 0) {
        fprintf(stderr, "warning: pthread_setaffinity_np(%s) failed: %d\n",
                name ? name : "thread", rc);
    }
}

/* Triple-buffered camera reader, shared by both eyes so they stay frame-
 * paired at capture time (rpicam-vid --sync). Same design as
 * mps_imt_rpicam_sbs_sender.c's CaptureCtx -- capture_loop never touches the
 * slot main is currently reading, so a slow encode on one eye cannot tear
 * the other eye's buffer. */
typedef struct {
    FILE*           left_pipe;
    FILE*           right_pipe;
    size_t          yuv_size;
    uint8_t*        buf[2][CAPTURE_SLOT_COUNT];
    int             write_slot;
    int             writer_slot;
    int             latest_slot;
    int             reader_slot;
    int             frame_seq;
    uint64_t        capture_timestamp_ns[CAPTURE_SLOT_COUNT];
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    volatile int    stop;
    int             error;
} CaptureCtx;

static int read_exact(FILE* pipe, uint8_t* data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        const size_t n = fread(data + offset, 1u, size - offset, pipe);
        if (n == 0u) {
            return ferror(pipe) ? -errno : -1;
        }
        offset += n;
    }
    return 0;
}

static void free_capture_buffers(CaptureCtx* c)
{
    int eye, slot;
    if (!c) {
        return;
    }
    for (eye = 0; eye < 2; ++eye) {
        for (slot = 0; slot < CAPTURE_SLOT_COUNT; ++slot) {
            free(c->buf[eye][slot]);
            c->buf[eye][slot] = NULL;
        }
    }
}

static int capture_choose_write_slot(CaptureCtx* c)
{
    int i;
    for (i = 0; i < CAPTURE_SLOT_COUNT; ++i) {
        const int slot = (c->write_slot + i) % CAPTURE_SLOT_COUNT;
        if (slot != c->latest_slot && slot != c->reader_slot && slot != c->writer_slot) {
            c->write_slot = slot;
            c->writer_slot = slot;
            return slot;
        }
    }
    return -1;
}

static void* capture_loop(void* arg)
{
    CaptureCtx* c = (CaptureCtx*)arg;
    while (!c->stop) {
        int slot;
        pthread_mutex_lock(&c->lock);
        slot = capture_choose_write_slot(c);
        pthread_mutex_unlock(&c->lock);
        if (slot < 0) {
            struct timespec delay;
            delay.tv_sec = 0;
            delay.tv_nsec = 1000000L;
            (void)nanosleep(&delay, NULL);
            continue;
        }

        if (read_exact(c->left_pipe, c->buf[0][slot], c->yuv_size) != 0 ||
            read_exact(c->right_pipe, c->buf[1][slot], c->yuv_size) != 0) {
            pthread_mutex_lock(&c->lock);
            c->error = 1;
            c->stop = 1;
            pthread_cond_signal(&c->cond);
            pthread_mutex_unlock(&c->lock);
            return NULL;
        }
        c->capture_timestamp_ns[slot] = now_ns();
        pthread_mutex_lock(&c->lock);
        c->latest_slot = slot;
        c->writer_slot = -1;
        c->write_slot = (slot + 1) % CAPTURE_SLOT_COUNT;
        c->frame_seq++;
        pthread_cond_signal(&c->cond);
        pthread_mutex_unlock(&c->lock);
    }
    return NULL;
}

static FILE* open_cam(uint32_t idx, uint32_t w, uint32_t h, uint32_t fps, const char* sync_role)
{
    char cmd[512];
    const char* sync_option = sync_role ? "--sync" : "";
    const char* sync_value = sync_role ? sync_role : "";
    snprintf(cmd, sizeof(cmd),
             "rpicam-vid --camera %u --width %u --height %u --framerate %u "
             "--codec yuv420 --timeout 0 --nopreview %s %s "
             "--output - 2>/tmp/cam%u.log",
             idx, w, h, fps, sync_option, sync_value, idx);
    return popen(cmd, "r");
}

/* One eye's encode + IMT transport. Kept in a struct so main() can drive
 * both eyes with the same code instead of duplicating the send block. */
typedef struct {
    const char* label;
    MpsX264Ctx* x264;
    ImtPacketizer pkt;
    SendCtx ctx;
    uint64_t frame_seq;
    uint64_t log_start_ns;
    uint64_t log_bytes;
    uint32_t log_frames;
} EyeStream;

static int eye_stream_init(EyeStream* eye, const char* label, const char* host, uint16_t port,
                           uint32_t w, uint32_t h, uint32_t fps, float crf)
{
    memset(eye, 0, sizeof(*eye));
    eye->label = label;

    eye->x264 = mps_x264_init(w, h, fps, crf);
    if (!eye->x264) {
        fprintf(stderr, "%s: mps_x264_init failed\n", label);
        return -1;
    }
    if (imt_packetizer_init(&eye->pkt, 0, 0) != 0) {
        fprintf(stderr, "%s: imt_packetizer_init failed\n", label);
        mps_x264_destroy(eye->x264);
        return -2;
    }

    eye->ctx.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (eye->ctx.fd < 0) {
        fprintf(stderr, "%s: socket failed: %s\n", label, strerror(errno));
        imt_packetizer_destroy(&eye->pkt);
        mps_x264_destroy(eye->x264);
        return -3;
    }
    eye->ctx.target_rate_bytes_per_sec = MPS_PACING_INITIAL_BYTES_PER_SEC;
    {
        const unsigned int rate = (unsigned int)eye->ctx.target_rate_bytes_per_sec;
        (void)setsockopt(eye->ctx.fd, SOL_SOCKET, SO_MAX_PACING_RATE, &rate, sizeof(rate));
    }
    memset(&eye->ctx.addr, 0, sizeof(eye->ctx.addr));
    eye->ctx.addr.sin_family = AF_INET;
    eye->ctx.addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &eye->ctx.addr.sin_addr) != 1) {
        fprintf(stderr, "%s: bad host %s\n", label, host);
        close(eye->ctx.fd);
        imt_packetizer_destroy(&eye->pkt);
        mps_x264_destroy(eye->x264);
        return -4;
    }
    if (bind_udp_source_port(eye->ctx.fd, port) != 0) {
        fprintf(stderr, "warning: %s UDP source port %u bind failed: %s\n",
                label, (unsigned)port, strerror(errno));
    }
    return 0;
}

static void eye_stream_destroy(EyeStream* eye)
{
    if (!eye) {
        return;
    }
    if (eye->ctx.fd >= 0) {
        close(eye->ctx.fd);
    }
    imt_packetizer_destroy(&eye->pkt);
    if (eye->x264) {
        mps_x264_destroy(eye->x264);
    }
    memset(eye, 0, sizeof(*eye));
}

/* Encode one eye's raw I420 buffer and send it as its own IMT frame. No
 * importance-map QP weighting (no combined-SBS ROI concept here) and no
 * weighted FEC -- imt_packetizer_send_frame's NULL chunk_weights_norm path
 * already means "uniform group-of-8", which is the documented safe
 * degradation (R2.5/R4.5) when no map is being produced. */
static void eye_stream_encode_and_send(EyeStream* eye, const uint8_t* i420,
                                       uint64_t capture_timestamp_ns)
{
    uint8_t* nal = NULL;
    int is_keyframe = 0;
    const MpsX264SliceInfo* slices = NULL;
    uint32_t slice_count = 0u;
    const int nal_size = mps_x264_encode(eye->x264, i420, NULL, NULL,
                                         &nal, &is_keyframe, &slices, &slice_count);
    if (nal_size <= 0) {
        return;
    }

    const uint16_t flags = is_keyframe ? (IMT_PACKET_FLAG_KEYFRAME | IMT_PACKET_FLAG_CODEC_CONFIG) : 0u;
    const int send_rc = imt_packetizer_send_frame(&eye->pkt, nal, (uint32_t)nal_size,
                                                  eye->frame_seq, capture_timestamp_ns,
                                                  flags, NULL, 0u, emit_datagram, &eye->ctx);
    if (send_rc != 0) {
        static uint64_t last_error_ns;
        const uint64_t now = now_ns();
        if (now - last_error_ns >= 1000000000ull) {
            fprintf(stderr, "%s: imt frame send failed: %d\n", eye->label, send_rc);
            last_error_ns = now;
        }
    }
    eye->frame_seq += 1u;

    if (eye->log_start_ns == 0u) {
        eye->log_start_ns = now_ns();
    }
    eye->log_bytes += (uint64_t)nal_size;
    eye->log_frames += 1u;
    {
        const uint64_t now = now_ns();
        if (now - eye->log_start_ns >= 1000000000ull) {
            const double seconds = (double)(now - eye->log_start_ns) / 1000000000.0;
            const double mbps = ((double)eye->log_bytes * 8.0) / (seconds * 1000000.0);
            const double fps_out = (double)eye->log_frames / seconds;
            fprintf(stderr, "%s: %.1f fps, %.2f Mbps\n", eye->label, fps_out, mbps);
            eye->log_start_ns = now;
            eye->log_bytes = 0u;
            eye->log_frames = 0u;
        }
    }
}

int main(int argc, char** argv)
{
    const char* host    = argc > 1 ? argv[1]                  : "192.168.137.1";
    const uint16_t port = argc > 2 ? (uint16_t)atoi(argv[2]) : 5004u;
    const uint32_t eye_w = argc > 3 ? (uint32_t)atoi(argv[3]) : 1280u;
    const uint32_t eye_h = argc > 4 ? (uint32_t)atoi(argv[4]) : 720u;
    const uint32_t fps   = argc > 5 ? (uint32_t)atoi(argv[5]) : 60u;
    const uint32_t left_cam  = argc > 6 ? (uint32_t)atoi(argv[6]) : 1u;
    const uint32_t right_cam = argc > 7 ? (uint32_t)atoi(argv[7]) : 0u;
    const char* crf_env = getenv("MPS_X264_CRF");
    /* Higher CRF than the SBS sender's default (14): two independent
     * higher-resolution streams cost roughly 2x the bitrate of one combined
     * SBS frame at the same per-pixel quality, so start a bit leaner and
     * let the operator tune via MPS_X264_CRF. */
    const float x264_crf = crf_env ? (float)atof(crf_env) : 20.0f;
    const char* camera_sync_env = getenv("MPS_CAMERA_SYNC");
    const int camera_sync_enabled =
        !camera_sync_env ||
        (strcmp(camera_sync_env, "0") != 0 && strcmp(camera_sync_env, "off") != 0 &&
         strcmp(camera_sync_env, "OFF") != 0);

    if (eye_w < 2u || eye_h < 2u || fps == 0u || left_cam == right_cam || port >= 0xFFFFu) {
        fprintf(stderr,
                "usage: %s [host] [port] [eye_w] [eye_h] [fps] [left_cam] [right_cam]\n"
                "  left eye -> port, right eye -> port+1\n"
                "  env MPS_X264_CRF (default 20), MPS_CAMERA_SYNC=0 to disable hw sync\n",
                argv[0]);
        return 1;
    }

    const size_t yuv_size = (size_t)eye_w * eye_h * 3u / 2u;

    CaptureCtx cap;
    int eye_idx, slot;
    memset(&cap, 0, sizeof(cap));
    cap.writer_slot = -1;
    cap.latest_slot = -1;
    cap.reader_slot = -1;
    cap.yuv_size = yuv_size;
    for (eye_idx = 0; eye_idx < 2; ++eye_idx) {
        for (slot = 0; slot < CAPTURE_SLOT_COUNT; ++slot) {
            cap.buf[eye_idx][slot] = (uint8_t*)malloc(yuv_size);
            if (!cap.buf[eye_idx][slot]) {
                fprintf(stderr, "malloc failed\n");
                free_capture_buffers(&cap);
                return 2;
            }
        }
    }

    EyeStream left, right;
    if (eye_stream_init(&left, "left", host, port, eye_w, eye_h, fps, x264_crf) != 0 ||
        eye_stream_init(&right, "right", host, (uint16_t)(port + 1u), eye_w, eye_h, fps, x264_crf) != 0) {
        eye_stream_destroy(&left);
        eye_stream_destroy(&right);
        free_capture_buffers(&cap);
        return 3;
    }

    cap.right_pipe = open_cam(right_cam, eye_w, eye_h, fps, camera_sync_enabled ? "client" : NULL);
    cap.left_pipe  = open_cam(left_cam,  eye_w, eye_h, fps, camera_sync_enabled ? "server" : NULL);
    if (!cap.left_pipe || !cap.right_pipe) {
        fprintf(stderr, "failed to start rpicam-vid (see /tmp/cam*.log)\n");
        if (cap.left_pipe) pclose(cap.left_pipe);
        if (cap.right_pipe) pclose(cap.right_pipe);
        eye_stream_destroy(&left);
        eye_stream_destroy(&right);
        free_capture_buffers(&cap);
        return 4;
    }

    fprintf(stderr,
            "mps-stereo-x264 -> %s left=%u right=%u  cam L%u/R%u  %ux%u @ %u fps  crf=%.1f  sync=%s\n",
            host, (unsigned)port, (unsigned)(port + 1u), left_cam, right_cam,
            eye_w, eye_h, fps, x264_crf, camera_sync_enabled ? "software" : "off");

    pthread_mutex_init(&cap.lock, NULL);
    pthread_cond_init(&cap.cond, NULL);
    pthread_t capture_tid;
    if (pthread_create(&capture_tid, NULL, capture_loop, &cap) != 0) {
        fprintf(stderr, "capture thread create failed\n");
        pclose(cap.left_pipe);
        pclose(cap.right_pipe);
        pthread_mutex_destroy(&cap.lock);
        pthread_cond_destroy(&cap.cond);
        eye_stream_destroy(&left);
        eye_stream_destroy(&right);
        free_capture_buffers(&cap);
        return 5;
    }
    {
        const int capture_cores[] = {0};
        set_thread_affinity_mask(capture_tid, capture_cores, 1u, "capture");
        /* One core each for the two encoders (mps_x264_init sets i_threads=1
         * per context), leaving core 0 for capture. Both encode calls happen
         * back-to-back on the main thread below rather than on separate
         * threads: x264_encoder_encode() for one 720p frame at ultrafast is
         * a few ms, so serialising the two is simpler than adding another
         * pair of worker threads/queues for a latency cost that stays well
         * under one frame interval even at 60fps. */
        const int main_cores[] = {1, 2, 3};
        set_thread_affinity_mask(pthread_self(), main_cores, 3u, "main");
    }

    int last_cap_seq = -1;
    for (;;) {
        int front;
        int cap_error;
        pthread_mutex_lock(&cap.lock);
        while ((cap.latest_slot < 0 || cap.frame_seq == last_cap_seq) && !cap.stop) {
            pthread_cond_wait(&cap.cond, &cap.lock);
        }
        cap_error = cap.error;
        if (!cap_error && cap.latest_slot >= 0) {
            front = cap.latest_slot;
            cap.reader_slot = front;
        } else {
            front = -1;
        }
        last_cap_seq = cap.frame_seq;
        pthread_mutex_unlock(&cap.lock);

        if (cap_error || front < 0) {
            fprintf(stderr, "capture thread failed (see /tmp/cam*.log)\n");
            break;
        }

        const uint64_t capture_timestamp_ns =
            cap.capture_timestamp_ns[front] != 0u ? cap.capture_timestamp_ns[front] : now_ns();
        eye_stream_encode_and_send(&left, cap.buf[0][front], capture_timestamp_ns);
        eye_stream_encode_and_send(&right, cap.buf[1][front], capture_timestamp_ns);

        pthread_mutex_lock(&cap.lock);
        if (cap.reader_slot == front) {
            cap.reader_slot = -1;
        }
        pthread_mutex_unlock(&cap.lock);
    }

    pclose(cap.left_pipe);
    pclose(cap.right_pipe);
    pthread_join(capture_tid, NULL);
    pthread_mutex_destroy(&cap.lock);
    pthread_cond_destroy(&cap.cond);
    eye_stream_destroy(&left);
    eye_stream_destroy(&right);
    free_capture_buffers(&cap);
    return 0;
}
