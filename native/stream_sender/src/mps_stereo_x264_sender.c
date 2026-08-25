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
 * independent IMT byte stream. Both eyes carry the same capture sequence;
 * each keeps its own FEC groups and configurable x264 GOP while persistent
 * worker threads encode the captured pair concurrently.
 */
#ifndef _WIN32
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#ifdef _WIN32
#error "mps_stereo_x264_sender targets Linux/Pi only."
#endif

#include "imt.h"
#include "mps_libcamera_stereo_capture.h"
#include "mps_x264_encoder.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAPTURE_SLOT_COUNT 3
#define MPS_SEND_BATCH_DATAGRAMS 16u
#define MPS_SEND_DATAGRAM_BYTES (IMT_WIRE_HEADER_SIZE + IMT_DEFAULT_MAX_PAYLOAD)
#ifndef SO_MAX_PACING_RATE
#define SO_MAX_PACING_RATE 47
#endif

typedef struct {
    int fd;
    struct sockaddr_in addr;
    uint64_t datagrams_sent;
    uint64_t bytes_sent;
    uint64_t send_failures;
    struct mmsghdr messages[MPS_SEND_BATCH_DATAGRAMS];
    struct iovec iov[MPS_SEND_BATCH_DATAGRAMS];
    uint8_t batch_data[MPS_SEND_BATCH_DATAGRAMS][MPS_SEND_DATAGRAM_BYTES];
    uint32_t batch_count;
} SendCtx;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int send_ctx_flush(SendCtx* ctx)
{
    uint32_t offset = 0u;
    const uint32_t count = ctx ? ctx->batch_count : 0u;
    if (!ctx || ctx->fd < 0) {
        return -1;
    }
    while (offset < count) {
        const int sent = sendmmsg(ctx->fd, &ctx->messages[offset], count - offset, 0);
        uint32_t i;
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            ctx->send_failures += count - offset;
            ctx->batch_count = 0u;
            return -1;
        }
        if (sent == 0) {
            ctx->send_failures += count - offset;
            ctx->batch_count = 0u;
            return -1;
        }
        for (i = 0u; i < (uint32_t)sent; ++i) {
            ctx->datagrams_sent += 1u;
            ctx->bytes_sent += (uint64_t)ctx->iov[offset + i].iov_len;
        }
        offset += (uint32_t)sent;
    }
    ctx->batch_count = 0u;
    return 0;
}

static int emit_datagram(const uint8_t* data, size_t len, void* user)
{
    SendCtx* ctx = (SendCtx*)user;
    uint32_t slot;
    if (!ctx || !data || len == 0u || len > MPS_SEND_DATAGRAM_BYTES) {
        return -1;
    }
    slot = ctx->batch_count;
    if (slot >= MPS_SEND_BATCH_DATAGRAMS) {
        return -1;
    }
    memcpy(ctx->batch_data[slot], data, len);
    memset(&ctx->messages[slot], 0, sizeof(ctx->messages[slot]));
    ctx->iov[slot].iov_base = ctx->batch_data[slot];
    ctx->iov[slot].iov_len = len;
    ctx->messages[slot].msg_hdr.msg_iov = &ctx->iov[slot];
    ctx->messages[slot].msg_hdr.msg_iovlen = 1u;
    ctx->batch_count += 1u;
    return ctx->batch_count == MPS_SEND_BATCH_DATAGRAMS ? send_ctx_flush(ctx) : 0;
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
    uint64_t        frame_seq;
    uint64_t        slot_frame_seq[CAPTURE_SLOT_COUNT];
    uint64_t        capture_timestamp_ns[CAPTURE_SLOT_COUNT];
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int             stop;
    int             error;
} CaptureCtx;

static int read_camera_pair(FILE* left_pipe, uint8_t* left_data,
                            FILE* right_pipe, uint8_t* right_data, size_t size)
{
    struct pollfd fds[2];
    uint8_t* data[2] = {left_data, right_data};
    size_t offsets[2] = {0u, 0u};
    int i;

    if (!left_pipe || !right_pipe || !left_data || !right_data || size == 0u) {
        return -1;
    }
    fds[0].fd = fileno(left_pipe);
    fds[1].fd = fileno(right_pipe);
    if (fds[0].fd < 0 || fds[1].fd < 0) {
        return -1;
    }

    while (offsets[0] < size || offsets[1] < size) {
        for (i = 0; i < 2; ++i) {
            fds[i].events = offsets[i] < size ? POLLIN : 0;
            fds[i].revents = 0;
        }
        if (poll(fds, 2u, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        for (i = 0; i < 2; ++i) {
            ssize_t n;
            if (offsets[i] >= size) {
                continue;
            }
            if ((fds[i].revents & (POLLERR | POLLNVAL)) != 0) {
                return -1;
            }
            if ((fds[i].revents & (POLLIN | POLLHUP)) == 0) {
                continue;
            }
            n = read(fds[i].fd, data[i] + offsets[i], size - offsets[i]);
            if (n > 0) {
                offsets[i] += (size_t)n;
            } else if (n == 0) {
                return -1;
            } else if (errno != EINTR && errno != EAGAIN) {
                return -errno;
            }
        }
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
    for (;;) {
        int slot;
        pthread_mutex_lock(&c->lock);
        if (c->stop) {
            pthread_mutex_unlock(&c->lock);
            break;
        }
        slot = capture_choose_write_slot(c);
        pthread_mutex_unlock(&c->lock);
        if (slot < 0) {
            struct timespec delay;
            delay.tv_sec = 0;
            delay.tv_nsec = 1000000L;
            (void)nanosleep(&delay, NULL);
            continue;
        }

        if (read_camera_pair(c->left_pipe, c->buf[0][slot],
                             c->right_pipe, c->buf[1][slot], c->yuv_size) != 0) {
            pthread_mutex_lock(&c->lock);
            c->error = 1;
            c->stop = 1;
            pthread_cond_signal(&c->cond);
            pthread_mutex_unlock(&c->lock);
            return NULL;
        }
        c->capture_timestamp_ns[slot] = now_ns();
        pthread_mutex_lock(&c->lock);
        c->frame_seq += 1u;
        c->slot_frame_seq[slot] = c->frame_seq;
        c->latest_slot = slot;
        c->writer_slot = -1;
        c->write_slot = (slot + 1) % CAPTURE_SLOT_COUNT;
        pthread_cond_signal(&c->cond);
        pthread_mutex_unlock(&c->lock);
    }
    return NULL;
}

static FILE* open_cam(uint32_t idx, uint32_t w, uint32_t h, uint32_t fps,
                      uint32_t buffer_count, const char* sync_role)
{
    char cmd[512];
    const char* sync_option = sync_role ? "--sync" : "";
    const char* sync_value = sync_role ? sync_role : "";
    snprintf(cmd, sizeof(cmd),
             "rpicam-vid --camera %u --width %u --height %u --framerate %u --buffer-count %u "
             "--codec yuv420 --timeout 0 --nopreview %s %s "
             "--output - 2>/tmp/cam%u.log",
             idx, w, h, fps, buffer_count, sync_option, sync_value, idx);
    {
        FILE* pipe = popen(cmd, "r");
        if (pipe) {
            (void)setvbuf(pipe, NULL, _IONBF, 0);
        }
        return pipe;
    }
}

/* One eye's encode + IMT transport. Kept in a struct so main() can drive
 * both eyes with the same code instead of duplicating the send block. */
typedef struct {
    const char* label;
    MpsX264Ctx* x264;
    ImtPacketizer pkt;
    SendCtx ctx;
    uint64_t log_start_ns;
    uint64_t log_bytes;
    uint64_t log_encode_ns;
    uint64_t log_send_ns;
    uint64_t last_work_ns;
    uint32_t log_frames;
    uint64_t last_error_ns;
} EyeStream;

static void eye_stream_destroy(EyeStream* eye);

static int eye_stream_init(EyeStream* eye, const char* label, const char* host, uint16_t port,
                           uint32_t w, uint32_t h, uint32_t fps, float crf,
                           uint32_t keyint, uint32_t kernel_pacing_mbps)
{
    MpsX264Options x264_options;
    memset(eye, 0, sizeof(*eye));
    eye->ctx.fd = -1;
    eye->label = label;

    mps_x264_default_options(&x264_options);
    x264_options.keyint = keyint;
    x264_options.threads = 1u;
    x264_options.slice_max_size = 0u;
    eye->x264 = mps_x264_init_ex(w, h, fps, crf, &x264_options);
    if (!eye->x264) {
        fprintf(stderr, "%s: mps_x264_init failed\n", label);
        eye_stream_destroy(eye);
        return -1;
    }
    if (imt_packetizer_init(&eye->pkt, 0, 0) != 0) {
        fprintf(stderr, "%s: imt_packetizer_init failed\n", label);
        eye_stream_destroy(eye);
        return -2;
    }

    eye->ctx.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (eye->ctx.fd < 0) {
        fprintf(stderr, "%s: socket failed: %s\n", label, strerror(errno));
        eye_stream_destroy(eye);
        return -3;
    }
    if (kernel_pacing_mbps > 0u) {
        const uint64_t rate64 = (uint64_t)kernel_pacing_mbps * 1000000ull / 8ull;
        const unsigned int rate = rate64 > 0xffffffffull ? 0xffffffffu : (unsigned int)rate64;
        if (setsockopt(eye->ctx.fd, SOL_SOCKET, SO_MAX_PACING_RATE,
                       &rate, sizeof(rate)) != 0) {
            fprintf(stderr, "warning: %s SO_MAX_PACING_RATE=%uMbps failed: %s\n",
                    label, kernel_pacing_mbps, strerror(errno));
        } else {
            unsigned int effective = 0u;
            socklen_t effective_size = sizeof(effective);
            if (getsockopt(eye->ctx.fd, SOL_SOCKET, SO_MAX_PACING_RATE,
                           &effective, &effective_size) == 0) {
                fprintf(stderr,
                        "%s: kernel pacing requested=%uMbps effective=%.2fMbps "
                        "(requires an fq qdisc to pace)\n",
                        label, kernel_pacing_mbps,
                        (double)effective * 8.0 / 1000000.0);
            }
        }
    }
    memset(&eye->ctx.addr, 0, sizeof(eye->ctx.addr));
    eye->ctx.addr.sin_family = AF_INET;
    eye->ctx.addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &eye->ctx.addr.sin_addr) != 1) {
        fprintf(stderr, "%s: bad host %s\n", label, host);
        eye_stream_destroy(eye);
        return -4;
    }
    if (bind_udp_source_port(eye->ctx.fd, port) != 0) {
        fprintf(stderr, "warning: %s UDP source port %u bind failed: %s\n",
                label, (unsigned)port, strerror(errno));
    }
    if (connect(eye->ctx.fd, (const struct sockaddr*)&eye->ctx.addr, sizeof(eye->ctx.addr)) != 0) {
        fprintf(stderr, "%s: UDP connect failed: %s\n", label, strerror(errno));
        eye_stream_destroy(eye);
        return -5;
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
    eye->ctx.fd = -1;
}

/* Encode one eye's raw I420 buffer and send it as its own IMT frame. No
 * importance-map QP weighting (no combined-SBS ROI concept here) and no
 * weighted FEC -- imt_packetizer_send_frame's NULL chunk_weights_norm path
 * already means "uniform group-of-8", which is the documented safe
 * degradation (R2.5/R4.5) when no map is being produced. */
static void eye_stream_encode_and_send(EyeStream* eye, const uint8_t* i420,
                                       uint64_t capture_timestamp_ns, uint64_t frame_seq)
{
    uint8_t* nal = NULL;
    int is_keyframe = 0;
    const MpsX264SliceInfo* slices = NULL;
    uint32_t slice_count = 0u;
    const uint64_t encode_start_ns = now_ns();
    const int nal_size = mps_x264_encode(eye->x264, i420, NULL, NULL,
                                         &nal, &is_keyframe, &slices, &slice_count);
    const uint64_t encode_end_ns = now_ns();
    if (nal_size <= 0) {
        return;
    }

    const uint16_t flags = is_keyframe ? (IMT_PACKET_FLAG_KEYFRAME | IMT_PACKET_FLAG_CODEC_CONFIG) : 0u;
    int send_rc = imt_packetizer_send_frame(&eye->pkt, nal, (uint32_t)nal_size,
                                            frame_seq, capture_timestamp_ns,
                                            flags, NULL, 0u, emit_datagram, &eye->ctx);
    if (send_rc == 0) {
        send_rc = send_ctx_flush(&eye->ctx);
    } else {
        /* Never transmit a partial tail after packetization aborted. */
        eye->ctx.batch_count = 0u;
    }
    const uint64_t send_end_ns = now_ns();
    eye->last_work_ns = send_end_ns - encode_start_ns;
    if (send_rc != 0) {
        if (send_end_ns - eye->last_error_ns >= 1000000000ull) {
            fprintf(stderr, "%s: imt frame send failed: %d\n", eye->label, send_rc);
            eye->last_error_ns = send_end_ns;
        }
    }

    if (eye->log_start_ns == 0u) {
        eye->log_start_ns = send_end_ns;
    }
    eye->log_bytes += (uint64_t)nal_size;
    eye->log_encode_ns += encode_end_ns - encode_start_ns;
    eye->log_send_ns += send_end_ns - encode_end_ns;
    eye->log_frames += 1u;
    {
        const uint64_t now = send_end_ns;
        if (now - eye->log_start_ns >= 1000000000ull) {
            const double seconds = (double)(now - eye->log_start_ns) / 1000000000.0;
            const double mbps = ((double)eye->log_bytes * 8.0) / (seconds * 1000000.0);
            const double fps_out = (double)eye->log_frames / seconds;
            const double encode_ms = (double)eye->log_encode_ns /
                                     ((double)eye->log_frames * 1000000.0);
            const double send_ms = (double)eye->log_send_ns /
                                   ((double)eye->log_frames * 1000000.0);
            fprintf(stderr, "%s: %.1f fps, %.2f Mbps, encode %.2f ms, send %.2f ms\n",
                    eye->label, fps_out, mbps, encode_ms, send_ms);
            eye->log_start_ns = now;
            eye->log_bytes = 0u;
            eye->log_encode_ns = 0u;
            eye->log_send_ns = 0u;
            eye->log_frames = 0u;
        }
    }
}

typedef struct {
    EyeStream* eye;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_cond_t done;
    const uint8_t* i420;
    uint64_t capture_timestamp_ns;
    uint64_t frame_seq;
    uint64_t generation;
    uint64_t completed_generation;
    int stop;
    int started;
} EyeWorker;

static void* eye_worker_loop(void* arg)
{
    EyeWorker* worker = (EyeWorker*)arg;
    for (;;) {
        const uint8_t* i420;
        uint64_t timestamp_ns;
        uint64_t frame_seq;
        uint64_t generation;

        pthread_mutex_lock(&worker->lock);
        while (!worker->stop && worker->generation == worker->completed_generation) {
            pthread_cond_wait(&worker->wake, &worker->lock);
        }
        if (worker->stop) {
            pthread_mutex_unlock(&worker->lock);
            break;
        }
        i420 = worker->i420;
        timestamp_ns = worker->capture_timestamp_ns;
        frame_seq = worker->frame_seq;
        generation = worker->generation;
        pthread_mutex_unlock(&worker->lock);

        eye_stream_encode_and_send(worker->eye, i420, timestamp_ns, frame_seq);

        pthread_mutex_lock(&worker->lock);
        worker->completed_generation = generation;
        pthread_cond_signal(&worker->done);
        pthread_mutex_unlock(&worker->lock);
    }
    return NULL;
}

static int eye_worker_init(EyeWorker* worker, EyeStream* eye)
{
    memset(worker, 0, sizeof(*worker));
    worker->eye = eye;
    if (pthread_mutex_init(&worker->lock, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&worker->wake, NULL) != 0) {
        pthread_mutex_destroy(&worker->lock);
        return -1;
    }
    if (pthread_cond_init(&worker->done, NULL) != 0) {
        pthread_cond_destroy(&worker->wake);
        pthread_mutex_destroy(&worker->lock);
        return -1;
    }
    if (pthread_create(&worker->thread, NULL, eye_worker_loop, worker) != 0) {
        pthread_cond_destroy(&worker->done);
        pthread_cond_destroy(&worker->wake);
        pthread_mutex_destroy(&worker->lock);
        return -1;
    }
    worker->started = 1;
    return 0;
}

static uint64_t eye_worker_submit(EyeWorker* worker, const uint8_t* i420,
                                  uint64_t timestamp_ns, uint64_t frame_seq)
{
    uint64_t generation;
    pthread_mutex_lock(&worker->lock);
    worker->i420 = i420;
    worker->capture_timestamp_ns = timestamp_ns;
    worker->frame_seq = frame_seq;
    worker->generation += 1u;
    generation = worker->generation;
    pthread_cond_signal(&worker->wake);
    pthread_mutex_unlock(&worker->lock);
    return generation;
}

static void eye_worker_wait(EyeWorker* worker, uint64_t generation)
{
    pthread_mutex_lock(&worker->lock);
    while (worker->completed_generation < generation) {
        pthread_cond_wait(&worker->done, &worker->lock);
    }
    pthread_mutex_unlock(&worker->lock);
}

static void eye_worker_destroy(EyeWorker* worker)
{
    if (!worker || !worker->started) {
        return;
    }
    pthread_mutex_lock(&worker->lock);
    worker->stop = 1;
    pthread_cond_signal(&worker->wake);
    pthread_mutex_unlock(&worker->lock);
    pthread_join(worker->thread, NULL);
    pthread_cond_destroy(&worker->done);
    pthread_cond_destroy(&worker->wake);
    pthread_mutex_destroy(&worker->lock);
    worker->started = 0;
}

static int parse_u32_value(const char* text, uint32_t* out_value)
{
    char* end = NULL;
    unsigned long value;
    if (!text || !*text || !out_value || text[0] == '-') {
        return -1;
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value > 0xfffffffful) {
        return -1;
    }
    *out_value = (uint32_t)value;
    return 0;
}

static uint32_t env_u32(const char* name, uint32_t fallback, uint32_t minimum, uint32_t maximum)
{
    const char* text = getenv(name);
    uint32_t value;
    if (!text || parse_u32_value(text, &value) != 0 || value < minimum || value > maximum) {
        return fallback;
    }
    return value;
}

typedef struct {
    ImtPace pace;
    float base_crf;
    float current_crf;
    float maximum_crf;
    uint32_t update_interval;
    uint32_t frames;
    int enabled;
} AdaptiveControl;

static void adaptive_control_init(AdaptiveControl* control, float base_crf, uint32_t fps)
{
    const uint32_t target_ms_default = (1000u + fps - 1u) / fps;
    const uint32_t target_ms = env_u32("MPS_ADAPTIVE_TARGET_MS",
                                       target_ms_default, 1u, 1000u);
    const uint32_t max_crf = env_u32("MPS_ADAPTIVE_CRF_MAX", 30u, 0u, 51u);
    memset(control, 0, sizeof(*control));
    control->base_crf = base_crf;
    control->current_crf = base_crf;
    control->maximum_crf = (float)max_crf > base_crf ? (float)max_crf : base_crf;
    control->update_interval = fps > 1u ? (fps + 1u) / 2u : 1u;
    control->enabled = env_u32("MPS_ADAPTIVE", 1u, 0u, 1u) != 0u &&
                       imt_pace_init(&control->pace,
                                     (int64_t)target_ms * 1000000ll) == 0;
    if (control->enabled) {
        fprintf(stderr,
                "adaptive CRF enabled: base=%.1f max=%.1f processing_target=%ums\n",
                control->base_crf, control->maximum_crf, target_ms);
    }
}

static void adaptive_control_update(AdaptiveControl* control,
                                    EyeStream* left, EyeStream* right)
{
    float next_crf;
    int32_t rate;
    const uint64_t work_ns = left->last_work_ns > right->last_work_ns
        ? left->last_work_ns : right->last_work_ns;
    if (!control->enabled || work_ns == 0u) {
        return;
    }
    rate = imt_pace_update(&control->pace, (int64_t)work_ns);
    if (rate < 0) {
        control->enabled = 0;
        return;
    }
    control->frames += 1u;
    if (control->frames < control->update_interval) {
        return;
    }
    control->frames = 0u;
    next_crf = control->current_crf;
    if (rate < (int32_t)(IMT_PACE_Q16_ONE * 9 / 10) &&
        next_crf < control->maximum_crf) {
        next_crf += 1.0f;
    } else if (rate > (int32_t)(IMT_PACE_Q16_ONE * 11 / 10) &&
               next_crf > control->base_crf) {
        next_crf -= 1.0f;
    }
    if (next_crf > control->maximum_crf) next_crf = control->maximum_crf;
    if (next_crf < control->base_crf) next_crf = control->base_crf;
    if (next_crf == control->current_crf) {
        return;
    }

    if (mps_x264_set_crf(left->x264, next_crf) != 0) {
        fprintf(stderr, "adaptive CRF reconfigure failed; disabling controller\n");
        control->enabled = 0;
        return;
    }
    if (mps_x264_set_crf(right->x264, next_crf) != 0) {
        (void)mps_x264_set_crf(left->x264, control->current_crf);
        fprintf(stderr, "adaptive CRF reconfigure failed; disabling controller\n");
        control->enabled = 0;
        return;
    }
    control->current_crf = next_crf;
    fprintf(stderr, "adaptive CRF -> %.1f (processing rate=%.2f, work=%.2fms)\n",
            next_crf, (double)rate / IMT_PACE_Q16_ONE,
            (double)work_ns / 1000000.0);
}

static int run_libcamera_capture(EyeStream* left, EyeStream* right,
                                 uint32_t left_cam, uint32_t right_cam,
                                 uint32_t width, uint32_t height, uint32_t fps,
                                 uint32_t buffer_count, int software_sync,
                                 uint32_t max_skew_ms,
                                 AdaptiveControl* adaptive)
{
    MpsLibcameraStereoCapture* capture = mps_libcamera_stereo_create(
        left_cam, right_cam, width, height, fps, buffer_count,
        (uint64_t)max_skew_ms * 1000000ull, software_sync);
    EyeWorker left_worker;
    EyeWorker right_worker;
    uint64_t max_observed_skew_ns = 0u;
    uint64_t pairs = 0u;
    uint64_t log_start_ns = now_ns();
    int result = 0;
    memset(&left_worker, 0, sizeof(left_worker));
    memset(&right_worker, 0, sizeof(right_worker));
    if (!capture) {
        fprintf(stderr, "direct libcamera open failed\n");
        return -1;
    }
    if (mps_libcamera_stereo_start(capture) != 0) {
        fprintf(stderr, "direct libcamera start failed\n");
        mps_libcamera_stereo_destroy(capture);
        return -2;
    }
    if (eye_worker_init(&left_worker, left) != 0 ||
        eye_worker_init(&right_worker, right) != 0) {
        fprintf(stderr, "direct capture encode worker create failed\n");
        eye_worker_destroy(&left_worker);
        eye_worker_destroy(&right_worker);
        mps_libcamera_stereo_destroy(capture);
        return -3;
    }
    {
        const long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
        if (cpu_count >= 4) {
            const int left_cores[] = {1};
            const int right_cores[] = {2};
            const int main_cores[] = {3};
            set_thread_affinity_mask(left_worker.thread, left_cores, 1u, "left encoder");
            set_thread_affinity_mask(right_worker.thread, right_cores, 1u, "right encoder");
            set_thread_affinity_mask(pthread_self(), main_cores, 1u, "dispatcher");
        }
    }

    for (;;) {
        MpsLibcameraStereoPair pair;
        const int acquire_rc = mps_libcamera_stereo_acquire_pair(capture, 1000u, &pair);
        if (acquire_rc == 0) {
            continue;
        }
        if (acquire_rc < 0) {
            fprintf(stderr, "direct libcamera capture failed: %d\n", acquire_rc);
            result = -4;
            break;
        }
        if (pair.frame_size != (size_t)width * height * 3u / 2u) {
            fprintf(stderr, "direct libcamera returned invalid I420 size\n");
            mps_libcamera_stereo_release_pair(capture);
            result = -5;
            break;
        }

        {
            const uint64_t left_job = eye_worker_submit(
                &left_worker, pair.left_i420, pair.capture_timestamp_ns,
                pair.frame_sequence);
            const uint64_t right_job = eye_worker_submit(
                &right_worker, pair.right_i420, pair.capture_timestamp_ns,
                pair.frame_sequence);
            eye_worker_wait(&left_worker, left_job);
            eye_worker_wait(&right_worker, right_job);
        }
        adaptive_control_update(adaptive, left, right);
        if (pair.timestamp_skew_ns > max_observed_skew_ns) {
            max_observed_skew_ns = pair.timestamp_skew_ns;
        }
        pairs += 1u;
        mps_libcamera_stereo_release_pair(capture);

        if (now_ns() - log_start_ns >= 1000000000ull) {
            fprintf(stderr,
                    "capture: pairs=%llu max_skew=%.3fms dropped=%llu backend=libcamera\n",
                    (unsigned long long)pairs,
                    (double)max_observed_skew_ns / 1000000.0,
                    (unsigned long long)mps_libcamera_stereo_dropped_frames(capture));
            pairs = 0u;
            max_observed_skew_ns = 0u;
            log_start_ns = now_ns();
        }
    }

    eye_worker_destroy(&left_worker);
    eye_worker_destroy(&right_worker);
    mps_libcamera_stereo_destroy(capture);
    return result;
}

int main(int argc, char** argv)
{
    const char* host = argc > 1 ? argv[1] : "192.168.137.1";
    uint32_t port_value = 5004u;
    uint32_t eye_w = 1280u;
    uint32_t eye_h = 720u;
    uint32_t fps = 60u;
    uint32_t left_cam = 1u;
    uint32_t right_cam = 0u;
    int args_valid = 1;
    const char* crf_env = getenv("MPS_X264_CRF");
    /* Higher CRF than the SBS sender's default (14): two independent
     * higher-resolution streams cost roughly 2x the bitrate of one combined
     * SBS frame at the same per-pixel quality, so start a bit leaner and
     * let the operator tune via MPS_X264_CRF. */
    float x264_crf = 20.0f;
    const char* camera_sync_env = getenv("MPS_CAMERA_SYNC");
    const int camera_sync_enabled =
        !camera_sync_env ||
        (strcmp(camera_sync_env, "0") != 0 && strcmp(camera_sync_env, "off") != 0 &&
         strcmp(camera_sync_env, "OFF") != 0);
    uint32_t keyint;
    uint32_t camera_buffers;
    uint32_t kernel_pacing_mbps;
    uint32_t camera_max_skew_ms;
    const char* capture_backend = getenv("MPS_CAPTURE_BACKEND");

    if (argc > 2 && parse_u32_value(argv[2], &port_value) != 0) args_valid = 0;
    if (argc > 3 && parse_u32_value(argv[3], &eye_w) != 0) args_valid = 0;
    if (argc > 4 && parse_u32_value(argv[4], &eye_h) != 0) args_valid = 0;
    if (argc > 5 && parse_u32_value(argv[5], &fps) != 0) args_valid = 0;
    if (argc > 6 && parse_u32_value(argv[6], &left_cam) != 0) args_valid = 0;
    if (argc > 7 && parse_u32_value(argv[7], &right_cam) != 0) args_valid = 0;
    if (crf_env) {
        char* end = NULL;
        const float parsed = strtof(crf_env, &end);
        if (end && *end == '\0' && parsed >= 0.0f && parsed <= 51.0f) {
            x264_crf = parsed;
        } else {
            fprintf(stderr, "warning: ignoring invalid MPS_X264_CRF=%s\n", crf_env);
        }
    }

    keyint = env_u32("MPS_X264_KEYINT", fps > 1u ? (fps + 1u) / 2u : 1u, 1u, 600u);
    camera_buffers = env_u32("MPS_CAMERA_BUFFERS", 6u, 2u, 32u);
    kernel_pacing_mbps = env_u32("MPS_KERNEL_PACING_MBPS", 0u, 0u, 10000u);
    camera_max_skew_ms = env_u32("MPS_CAMERA_MAX_SKEW_MS", 5u, 0u, 1000u);
    if (!capture_backend || !*capture_backend) {
        capture_backend = "libcamera";
    }

    if (!args_valid || port_value == 0u || port_value >= 0xFFFFu ||
        eye_w < 2u || eye_h < 2u || (eye_w & 1u) != 0u || (eye_h & 1u) != 0u ||
        fps == 0u || fps > 1000u || left_cam == right_cam ||
        (strcmp(capture_backend, "rpicam") != 0 &&
         strcmp(capture_backend, "libcamera") != 0)) {
        fprintf(stderr,
                "usage: %s [host] [port] [eye_w] [eye_h] [fps] [left_cam] [right_cam]\n"
                "  left eye -> port, right eye -> port+1\n"
                "  eye_w/eye_h must be even\n"
                "  env MPS_X264_CRF=20, MPS_X264_KEYINT=fps/2, MPS_CAMERA_BUFFERS=6,\n"
                "      MPS_KERNEL_PACING_MBPS=0, MPS_CAMERA_SYNC=0 to disable software sync,\n"
                "      MPS_CAPTURE_BACKEND=rpicam|libcamera, MPS_CAMERA_MAX_SKEW_MS=5,\n"
                "      MPS_ADAPTIVE=1, MPS_ADAPTIVE_TARGET_MS=ceil(1000/fps), "
                "MPS_ADAPTIVE_CRF_MAX=30\n",
                argv[0]);
        return 1;
    }
    const uint16_t port = (uint16_t)port_value;

    const size_t yuv_size = (size_t)eye_w * eye_h * 3u / 2u;

    CaptureCtx cap;
    int eye_idx, slot;
    memset(&cap, 0, sizeof(cap));
    cap.writer_slot = -1;
    cap.latest_slot = -1;
    cap.reader_slot = -1;
    cap.yuv_size = yuv_size;

    EyeStream left, right;
    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));
    left.ctx.fd = -1;
    right.ctx.fd = -1;
    if (eye_stream_init(&left, "left", host, port, eye_w, eye_h, fps, x264_crf,
                        keyint, kernel_pacing_mbps) != 0 ||
        eye_stream_init(&right, "right", host, (uint16_t)(port + 1u), eye_w, eye_h, fps,
                        x264_crf, keyint, kernel_pacing_mbps) != 0) {
        eye_stream_destroy(&left);
        eye_stream_destroy(&right);
        free_capture_buffers(&cap);
        return 3;
    }
    AdaptiveControl adaptive;
    adaptive_control_init(&adaptive, x264_crf, fps);

    if (strcmp(capture_backend, "libcamera") == 0) {
        int capture_rc;
        fprintf(stderr,
                "mps-stereo-x264 -> %s left=%u right=%u cam L%u/R%u %ux%u @ %u fps "
                "crf=%.1f keyint=%u buffers=%u pacing=%uMbps sync=%s backend=libcamera "
                "max_skew=%ums\n",
                host, (unsigned)port, (unsigned)(port + 1u), left_cam, right_cam,
                eye_w, eye_h, fps, x264_crf, keyint, camera_buffers,
                kernel_pacing_mbps, camera_sync_enabled ? "software" : "off",
                camera_max_skew_ms);
        capture_rc = run_libcamera_capture(&left, &right, left_cam, right_cam,
                                           eye_w, eye_h, fps, camera_buffers,
                                           camera_sync_enabled, camera_max_skew_ms,
                                           &adaptive);
        eye_stream_destroy(&left);
        eye_stream_destroy(&right);
        return capture_rc == 0 ? 0 : 4;
    }

    for (eye_idx = 0; eye_idx < 2; ++eye_idx) {
        for (slot = 0; slot < CAPTURE_SLOT_COUNT; ++slot) {
            cap.buf[eye_idx][slot] = (uint8_t*)malloc(yuv_size);
            if (!cap.buf[eye_idx][slot]) {
                fprintf(stderr, "malloc failed\n");
                eye_stream_destroy(&left);
                eye_stream_destroy(&right);
                free_capture_buffers(&cap);
                return 2;
            }
        }
    }

    cap.right_pipe = open_cam(right_cam, eye_w, eye_h, fps, camera_buffers,
                              camera_sync_enabled ? "client" : NULL);
    cap.left_pipe  = open_cam(left_cam, eye_w, eye_h, fps, camera_buffers,
                              camera_sync_enabled ? "server" : NULL);
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
            "mps-stereo-x264 -> %s left=%u right=%u  cam L%u/R%u  %ux%u @ %u fps  "
            "crf=%.1f keyint=%u buffers=%u pacing=%uMbps sync=%s backend=rpicam\n",
            host, (unsigned)port, (unsigned)(port + 1u), left_cam, right_cam,
            eye_w, eye_h, fps, x264_crf, keyint, camera_buffers, kernel_pacing_mbps,
            camera_sync_enabled ? "software" : "off");

    pthread_mutex_init(&cap.lock, NULL);
    pthread_cond_init(&cap.cond, NULL);
    EyeWorker left_worker, right_worker;
    memset(&left_worker, 0, sizeof(left_worker));
    memset(&right_worker, 0, sizeof(right_worker));
    if (eye_worker_init(&left_worker, &left) != 0 ||
        eye_worker_init(&right_worker, &right) != 0) {
        fprintf(stderr, "encode worker create failed\n");
        eye_worker_destroy(&left_worker);
        eye_worker_destroy(&right_worker);
        pclose(cap.left_pipe);
        pclose(cap.right_pipe);
        pthread_mutex_destroy(&cap.lock);
        pthread_cond_destroy(&cap.cond);
        eye_stream_destroy(&left);
        eye_stream_destroy(&right);
        free_capture_buffers(&cap);
        return 5;
    }
    pthread_t capture_tid;
    if (pthread_create(&capture_tid, NULL, capture_loop, &cap) != 0) {
        fprintf(stderr, "capture thread create failed\n");
        pclose(cap.left_pipe);
        pclose(cap.right_pipe);
        pthread_mutex_destroy(&cap.lock);
        pthread_cond_destroy(&cap.cond);
        eye_worker_destroy(&left_worker);
        eye_worker_destroy(&right_worker);
        eye_stream_destroy(&left);
        eye_stream_destroy(&right);
        free_capture_buffers(&cap);
        return 5;
    }
    {
        const long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
        if (cpu_count >= 4) {
            const int capture_cores[] = {0};
            const int left_cores[] = {1};
            const int right_cores[] = {2};
            const int main_cores[] = {3};
            set_thread_affinity_mask(capture_tid, capture_cores, 1u, "capture");
            set_thread_affinity_mask(left_worker.thread, left_cores, 1u, "left encoder");
            set_thread_affinity_mask(right_worker.thread, right_cores, 1u, "right encoder");
            set_thread_affinity_mask(pthread_self(), main_cores, 1u, "dispatcher");
        }
    }

    uint64_t last_cap_seq = 0u;
    uint64_t skipped_capture_frames = 0u;
    for (;;) {
        int front;
        int cap_error;
        uint64_t frame_seq;
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
        frame_seq = front >= 0 ? cap.slot_frame_seq[front] : cap.frame_seq;
        pthread_mutex_unlock(&cap.lock);

        if (cap_error || front < 0) {
            fprintf(stderr, "capture thread failed (see /tmp/cam*.log)\n");
            break;
        }

        const uint64_t capture_timestamp_ns =
            cap.capture_timestamp_ns[front] != 0u ? cap.capture_timestamp_ns[front] : now_ns();
        if (last_cap_seq != 0u && frame_seq > last_cap_seq + 1u) {
            skipped_capture_frames += frame_seq - last_cap_seq - 1u;
        }
        last_cap_seq = frame_seq;
        {
            const uint64_t left_job = eye_worker_submit(&left_worker, cap.buf[0][front],
                                                        capture_timestamp_ns, frame_seq);
            const uint64_t right_job = eye_worker_submit(&right_worker, cap.buf[1][front],
                                                          capture_timestamp_ns, frame_seq);
            eye_worker_wait(&left_worker, left_job);
            eye_worker_wait(&right_worker, right_job);
        }
        adaptive_control_update(&adaptive, &left, &right);

        pthread_mutex_lock(&cap.lock);
        if (cap.reader_slot == front) {
            cap.reader_slot = -1;
        }
        pthread_mutex_unlock(&cap.lock);
    }

    fprintf(stderr, "capture frames skipped before encode: %llu\n",
            (unsigned long long)skipped_capture_frames);
    eye_worker_destroy(&left_worker);
    eye_worker_destroy(&right_worker);
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
