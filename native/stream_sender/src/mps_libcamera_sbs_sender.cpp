#include "mps_udp_sender.h"
#include "mps_i420_sbs.h"
#include "mps_nv12_sbs.h"
#include "mps_latency_gate.h"

#include <libcamera/framebuffer_allocator.h>
#include <libcamera/formats.h>
#include <libcamera/libcamera.h>

#if defined(MPS_ENABLE_GSTREAMER)
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#endif

#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

uint64_t now_ns()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

uint8_t clamp_u8(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<uint8_t>(value);
}

void yuv420_eye_to_rgba_sbs(const uint8_t* yuv,
                            uint8_t* rgba,
                            uint32_t eye_width,
                            uint32_t eye_height,
                            uint32_t dest_x_offset)
{
    const size_t y_plane_size = static_cast<size_t>(eye_width) * eye_height;
    const uint8_t* y_plane = yuv;
    const uint8_t* u_plane = yuv + y_plane_size;
    const uint8_t* v_plane = u_plane + y_plane_size / 4u;
    const uint32_t sbs_width = eye_width * 2u;

    for (uint32_t y = 0; y < eye_height; ++y) {
        for (uint32_t x = 0; x < eye_width; ++x) {
            const uint32_t chroma_index = (y / 2u) * (eye_width / 2u) + (x / 2u);
            const int yy = static_cast<int>(y_plane[static_cast<size_t>(y) * eye_width + x]);
            const int uu = static_cast<int>(u_plane[chroma_index]) - 128;
            const int vv = static_cast<int>(v_plane[chroma_index]) - 128;
            const int c = yy - 16;
            const int r = (298 * c + 409 * vv + 128) >> 8;
            const int g = (298 * c - 100 * uu - 208 * vv + 128) >> 8;
            const int b = (298 * c + 516 * uu + 128) >> 8;
            const size_t out = (static_cast<size_t>(y) * sbs_width + dest_x_offset + x) * 4u;

            rgba[out + 0u] = clamp_u8(r);
            rgba[out + 1u] = clamp_u8(g);
            rgba[out + 2u] = clamp_u8(b);
            rgba[out + 3u] = 255u;
        }
    }
}

#if defined(MPS_ENABLE_GSTREAMER)
class H264Encoder {
public:
    ~H264Encoder()
    {
        destroy();
    }

    int init(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate_kbps, bool prefer_software, bool use_nv12_input)
    {
        width_ = width;
        height_ = height;
        fps_ = fps ? fps : 30u;
        frame_duration_ns_ = 1000000000ull / fps_;
        use_nv12_input_ = use_nv12_input;

        gst_init(nullptr, nullptr);
        if (prefer_software) {
            if (tryCreatePipeline(width, height, fps_, bitrate_kbps, false) != 0) {
                return -1;
            }
        } else if (tryCreatePipeline(width, height, fps_, bitrate_kbps, true) != 0 &&
                   tryCreatePipeline(width, height, fps_, bitrate_kbps, false) != 0) {
            return -1;
        }

        GstStateChangeReturn state_rc = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (state_rc == GST_STATE_CHANGE_FAILURE) {
            destroy();
            return -2;
        }
        return 0;
    }

    int pushFrame(const uint8_t* i420, size_t size, uint64_t frame_index)
    {
        if (!appsrc_ || !i420 || size == 0u) {
            return -1;
        }

        GstBuffer* buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
        if (!buffer) {
            return -2;
        }

        gst_buffer_fill(buffer, 0, i420, size);
        GST_BUFFER_PTS(buffer) = frame_index * frame_duration_ns_;
        GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);
        GST_BUFFER_DURATION(buffer) = frame_duration_ns_;

        GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer);
        return flow == GST_FLOW_OK ? 0 : -3;
    }

    bool pullFrame(std::vector<uint8_t>* out)
    {
        if (!appsink_ || !out) {
            return false;
        }

        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink_), 0);
        if (!sample) {
            return false;
        }

        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstMapInfo map{};
        bool ok = false;
        if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            out->assign(map.data, map.data + map.size);
            gst_buffer_unmap(buffer, &map);
            ok = !out->empty();
        }
        gst_sample_unref(sample);
        return ok;
    }

private:
    int tryCreatePipeline(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate_kbps, bool hardware)
    {
        destroy();

        char description[2048];
        const char* input_format = use_nv12_input_ ? "NV12" : "I420";
        if (hardware) {
            std::snprintf(description,
                          sizeof(description),
                          "appsrc name=src is-live=true block=false format=time do-timestamp=false "
                          "max-buffers=1 max-bytes=0 max-time=0 leaky-type=downstream "
                          "caps=video/x-raw,format=%s,width=%u,height=%u,framerate=%u/1 "
                          "! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream "
                          "! v4l2h264enc extra-controls=\"controls,video_bitrate=%u\" "
                          "! h264parse config-interval=-1 "
                          "! video/x-h264,stream-format=byte-stream,alignment=au "
                          "! appsink name=sink emit-signals=false sync=false max-buffers=1 drop=true",
                          input_format,
                          width,
                          height,
                          fps,
                          bitrate_kbps * 1000u);
        } else {
            std::snprintf(description,
                          sizeof(description),
                          "appsrc name=src is-live=true block=false format=time do-timestamp=false "
                          "max-buffers=1 max-bytes=0 max-time=0 leaky-type=downstream "
                          "caps=video/x-raw,format=%s,width=%u,height=%u,framerate=%u/1 "
                          "! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream "
                          "! videoconvert "
                          "! x264enc tune=zerolatency speed-preset=ultrafast bitrate=%u key-int-max=%u bframes=0 "
                          "byte-stream=true aud=true cabac=false sliced-threads=true rc-lookahead=0 sync-lookahead=0 ref=1 "
                          "! h264parse config-interval=-1 "
                          "! video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline "
                          "! appsink name=sink emit-signals=false sync=false max-buffers=1 drop=true",
                          input_format,
                          width,
                          height,
                          fps,
                          bitrate_kbps,
                          fps);
        }

        GError* error = nullptr;
        pipeline_ = gst_parse_launch(description, &error);
        if (!pipeline_) {
            if (error) {
                std::fprintf(stderr, "GStreamer pipeline creation failed: %s\n", error->message);
                g_error_free(error);
            }
            return -1;
        }

        appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "src");
        appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
        if (!appsrc_ || !appsink_) {
            destroy();
            return -2;
        }

        std::fprintf(stderr, "H.264 encoder: %s\n", hardware ? "v4l2h264enc" : "x264enc");
        return 0;
    }

    void destroy()
    {
        if (pipeline_) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
        }
        if (appsrc_) {
            gst_object_unref(appsrc_);
            appsrc_ = nullptr;
        }
        if (appsink_) {
            gst_object_unref(appsink_);
            appsink_ = nullptr;
        }
        if (pipeline_) {
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
    }

    GstElement* pipeline_ = nullptr;
    GstElement* appsrc_ = nullptr;
    GstElement* appsink_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t fps_ = 30;
    uint64_t frame_duration_ns_ = 33333333ull;
    bool use_nv12_input_ = false;
};
#endif

struct PlaneMapping {
    void* mapped_address = nullptr;
    void* data_address = nullptr;
    size_t mapped_length = 0;
    size_t data_length = 0;
};

struct BufferMapping {
    std::vector<PlaneMapping> planes;
};

struct LatestFrame {
    std::vector<uint8_t> yuv;
    uint64_t timestamp_ns = 0;
    uint64_t sequence = 0;
    bool valid = false;
};

class CameraEye {
public:
    CameraEye(uint32_t camera_index,
              uint32_t width,
              uint32_t height,
              const char* name,
              std::condition_variable* notify_cv,
              std::mutex* notify_mutex)
        : camera_index_(camera_index),
          width_(width),
          height_(height),
          name_(name),
          notify_cv_(notify_cv),
          notify_mutex_(notify_mutex)
    {
        latest_.yuv.resize(yuv_size());
    }

    ~CameraEye()
    {
        stop();
        unmapBuffers();
    }

    int open(libcamera::CameraManager& manager)
    {
        const std::vector<std::shared_ptr<libcamera::Camera>>& cameras = manager.cameras();
        if (camera_index_ >= cameras.size()) {
            std::fprintf(stderr, "%s camera index %u out of range, available=%zu\n", name_, camera_index_, cameras.size());
            return -1;
        }

        camera_ = cameras[camera_index_];
        std::fprintf(stderr, "%s camera[%u]: %s\n", name_, camera_index_, camera_->id().c_str());

        int rc = camera_->acquire();
        if (rc != 0) {
            std::fprintf(stderr, "%s camera acquire failed: %d\n", name_, rc);
            return rc;
        }
        acquired_ = true;

        config_ = camera_->generateConfiguration({libcamera::StreamRole::VideoRecording});
        if (!config_ || config_->empty()) {
            std::fprintf(stderr, "%s generateConfiguration failed\n", name_);
            return -2;
        }

        libcamera::StreamConfiguration& stream_config = config_->at(0);
        stream_config.pixelFormat = libcamera::formats::YUV420;
        stream_config.size = libcamera::Size(width_, height_);
        stream_config.bufferCount = 4;

        const libcamera::CameraConfiguration::Status status = config_->validate();
        if (status == libcamera::CameraConfiguration::Invalid) {
            std::fprintf(stderr, "%s camera config invalid\n", name_);
            return -3;
        }
        if (stream_config.pixelFormat != libcamera::formats::YUV420) {
            std::fprintf(stderr,
                         "%s YUV420 was adjusted to %s; unsupported in this sender\n",
                         name_,
                         stream_config.pixelFormat.toString().c_str());
            return -4;
        }

        width_ = stream_config.size.width;
        height_ = stream_config.size.height;
        stride_ = stream_config.stride;
        std::fprintf(stderr,
                     "%s configured: %ux%u stride=%u format=%s buffers=%u\n",
                     name_,
                     width_,
                     height_,
                     stride_,
                     stream_config.pixelFormat.toString().c_str(),
                     stream_config.bufferCount);

        rc = camera_->configure(config_.get());
        if (rc != 0) {
            std::fprintf(stderr, "%s camera configure failed: %d\n", name_, rc);
            return rc;
        }
        stream_ = config_->at(0).stream();
        if (!stream_) {
            std::fprintf(stderr, "%s active stream is null after configure\n", name_);
            return -4;
        }

        allocator_ = std::make_unique<libcamera::FrameBufferAllocator>(camera_);
        rc = allocator_->allocate(stream_);
        if (rc < 0) {
            std::fprintf(stderr, "%s buffer allocation failed: %d\n", name_, rc);
            return rc;
        }

        for (const std::unique_ptr<libcamera::FrameBuffer>& buffer : allocator_->buffers(stream_)) {
            rc = mapBuffer(buffer.get());
            if (rc != 0) {
                return rc;
            }

            std::unique_ptr<libcamera::Request> request = camera_->createRequest();
            if (!request) {
                return -5;
            }

            rc = request->addBuffer(stream_, buffer.get());
            if (rc != 0) {
                std::fprintf(stderr, "%s addBuffer failed: %d\n", name_, rc);
                return rc;
            }
            requests_.push_back(std::move(request));
        }

        camera_->requestCompleted.connect(this, &CameraEye::requestComplete);
        return 0;
    }

    int start()
    {
        if (!camera_) {
            return -1;
        }

        running_.store(true);
        int rc = camera_->start();
        if (rc != 0) {
            std::fprintf(stderr, "%s camera start failed: %d\n", name_, rc);
            running_.store(false);
            return rc;
        }
        started_ = true;

        for (std::unique_ptr<libcamera::Request>& request : requests_) {
            rc = camera_->queueRequest(request.get());
            if (rc != 0) {
                std::fprintf(stderr, "%s queueRequest failed: %d\n", name_, rc);
                return rc;
            }
        }
        return 0;
    }

    void stop()
    {
        running_.store(false);
        if (camera_) {
            camera_->requestCompleted.disconnect(this, &CameraEye::requestComplete);
        }
        if (camera_ && started_) {
            camera_->stop();
            started_ = false;
        }
        if (camera_ && acquired_) {
            camera_->release();
            acquired_ = false;
        }
    }

    bool copyLatest(LatestFrame* out)
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (!latest_.valid || !out) {
            return false;
        }
        *out = latest_;
        return true;
    }

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    size_t yuv_size() const { return static_cast<size_t>(width_) * height_ * 3u / 2u; }

private:
    int mapBuffer(libcamera::FrameBuffer* buffer)
    {
        BufferMapping mapping;
        mapping.planes.reserve(buffer->planes().size());

        const long page_size = sysconf(_SC_PAGESIZE);
        if (page_size <= 0) {
            return -1;
        }

        for (const libcamera::FrameBuffer::Plane& plane : buffer->planes()) {
            const off_t page_mask = static_cast<off_t>(page_size - 1);
            const off_t map_offset = static_cast<off_t>(plane.offset) & ~page_mask;
            const size_t offset_delta = static_cast<size_t>(static_cast<off_t>(plane.offset) - map_offset);
            const size_t map_length = plane.length + offset_delta;
            void* mapped_address = mmap(nullptr, map_length, PROT_READ, MAP_SHARED, plane.fd.get(), map_offset);
            if (mapped_address == MAP_FAILED) {
                std::fprintf(stderr,
                             "%s mmap failed fd=%d offset=%u aligned_offset=%lld length=%u errno=%d (%s)\n",
                             name_,
                             plane.fd.get(),
                             plane.offset,
                             static_cast<long long>(map_offset),
                             plane.length,
                             errno,
                             std::strerror(errno));
                return -1;
            }
            mapping.planes.push_back({
                mapped_address,
                static_cast<uint8_t*>(mapped_address) + offset_delta,
                map_length,
                plane.length,
            });
        }

        mappings_.emplace(buffer, std::move(mapping));
        return 0;
    }

    void unmapBuffers()
    {
        for (auto& item : mappings_) {
            for (PlaneMapping& plane : item.second.planes) {
                if (plane.mapped_address && plane.mapped_address != MAP_FAILED) {
                    munmap(plane.mapped_address, plane.mapped_length);
                }
            }
        }
        mappings_.clear();
    }

    bool copyYuv420(libcamera::FrameBuffer* buffer, uint8_t* out)
    {
        auto mapping_it = mappings_.find(buffer);
        if (mapping_it == mappings_.end() || mapping_it->second.planes.size() < 3u) {
            std::fprintf(stderr, "%s expected 3-plane YUV420 buffer\n", name_);
            return false;
        }

        const BufferMapping& mapping = mapping_it->second;
        const uint8_t* y_plane = static_cast<const uint8_t*>(mapping.planes[0].data_address);
        const uint8_t* u_plane = static_cast<const uint8_t*>(mapping.planes[1].data_address);
        const uint8_t* v_plane = static_cast<const uint8_t*>(mapping.planes[2].data_address);
        const uint32_t y_stride = stride_ ? stride_ : width_;
        const uint32_t uv_stride = y_stride / 2u;
        uint8_t* y_out = out;
        uint8_t* u_out = y_out + static_cast<size_t>(width_) * height_;
        uint8_t* v_out = u_out + static_cast<size_t>(width_ / 2u) * (height_ / 2u);

        for (uint32_t row = 0; row < height_; ++row) {
            std::memcpy(y_out + static_cast<size_t>(row) * width_, y_plane + static_cast<size_t>(row) * y_stride, width_);
        }

        for (uint32_t row = 0; row < height_ / 2u; ++row) {
            std::memcpy(u_out + static_cast<size_t>(row) * (width_ / 2u), u_plane + static_cast<size_t>(row) * uv_stride, width_ / 2u);
            std::memcpy(v_out + static_cast<size_t>(row) * (width_ / 2u), v_plane + static_cast<size_t>(row) * uv_stride, width_ / 2u);
        }

        return true;
    }

    void requestComplete(libcamera::Request* request)
    {
        if (!running_.load()) {
            return;
        }
        if (request->status() == libcamera::Request::RequestCancelled) {
            return;
        }

        libcamera::FrameBuffer* buffer = request->findBuffer(stream_);
        if (buffer) {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            if (copyYuv420(buffer, latest_.yuv.data())) {
                latest_.timestamp_ns = buffer->metadata().timestamp;
                latest_.sequence += 1u;
                latest_.valid = true;
            }
        }

        if (notify_cv_ && notify_mutex_) {
            std::lock_guard<std::mutex> notify_lock(*notify_mutex_);
            notify_cv_->notify_all();
        }

        request->reuse(libcamera::Request::ReuseBuffers);
        if (running_.load()) {
            camera_->queueRequest(request);
        }
    }

    uint32_t camera_index_;
    uint32_t width_;
    uint32_t height_;
    uint32_t stride_ = 0;
    const char* name_;
    std::shared_ptr<libcamera::Camera> camera_;
    std::unique_ptr<libcamera::CameraConfiguration> config_;
    libcamera::Stream* stream_ = nullptr;
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
    std::vector<std::unique_ptr<libcamera::Request>> requests_;
    std::unordered_map<libcamera::FrameBuffer*, BufferMapping> mappings_;
    std::mutex frame_mutex_;
    LatestFrame latest_;
    std::atomic<bool> running_{false};
    bool started_ = false;
    bool acquired_ = false;
    std::condition_variable* notify_cv_;
    std::mutex* notify_mutex_;
};

uint64_t abs_diff_u64(uint64_t a, uint64_t b)
{
    return a > b ? a - b : b - a;
}

} // namespace

int main(int argc, char** argv)
{
    const char* host = argc > 1 ? argv[1] : "192.168.137.1";
    const uint16_t port = argc > 2 ? static_cast<uint16_t>(std::atoi(argv[2])) : 5004u;
    uint32_t eye_width = argc > 3 ? static_cast<uint32_t>(std::atoi(argv[3])) : 640u;
    uint32_t eye_height = argc > 4 ? static_cast<uint32_t>(std::atoi(argv[4])) : 360u;
    const uint32_t fps = argc > 5 ? static_cast<uint32_t>(std::atoi(argv[5])) : 30u;
    const uint32_t left_camera = argc > 6 ? static_cast<uint32_t>(std::atoi(argv[6])) : 1u;
    const uint32_t right_camera = argc > 7 ? static_cast<uint32_t>(std::atoi(argv[7])) : 0u;
    const uint64_t max_skew_ns = argc > 8 ? static_cast<uint64_t>(std::strtoull(argv[8], nullptr, 10)) * 1000000ull : 8000000ull;
    const std::string codec = argc > 9 ? argv[9] : "raw";
    const uint32_t bitrate_kbps = argc > 10 ? static_cast<uint32_t>(std::atoi(argv[10])) : 8000u;
    const std::string encoder_mode = argc > 11 ? argv[11] : "auto";
    const uint32_t max_pending_frames = argc > 12 ? static_cast<uint32_t>(std::atoi(argv[12])) : 1u;
    const uint64_t max_encode_age_ns = argc > 13
        ? static_cast<uint64_t>(std::strtoull(argv[13], nullptr, 10)) * 1000000ull
        : (fps ? 3000000000ull / static_cast<uint64_t>(fps) : 100000000ull);
    const std::string sbs_format = argc > 14 ? argv[14] : "i420";
    const bool use_h264 = codec == "h264" || codec == "H264";
    const bool prefer_software_encoder = encoder_mode == "software" || encoder_mode == "x264";
    const bool use_nv12_pack = sbs_format == "nv12" || sbs_format == "NV12";
    std::mutex notify_mutex;
    std::condition_variable notify_cv;
    MpsUdpSender sender{};
    int rc = 0;

    if (eye_width < 160u || eye_height < 120u || (eye_width & 1u) != 0u || (eye_height & 1u) != 0u || fps == 0u) {
        std::fprintf(stderr,
                     "usage: %s [host] [port] [eye_width] [eye_height] [fps] [left_camera] [right_camera] [max_skew_ms] [raw|h264] [bitrate_kbps] [auto|software] [max_pending_frames] [max_encode_age_ms] [i420|nv12]\n",
                     argv[0]);
        return 1;
    }
#if !defined(MPS_ENABLE_GSTREAMER)
    if (use_h264) {
        std::fprintf(stderr, "this build does not include GStreamer/H.264 support\n");
        return 1;
    }
#endif
    libcamera::CameraManager manager;
    rc = manager.start();
    if (rc != 0) {
        std::fprintf(stderr, "CameraManager start failed: %d\n", rc);
        return 2;
    }

    std::fprintf(stderr, "available cameras: %zu\n", manager.cameras().size());
    for (size_t i = 0; i < manager.cameras().size(); ++i) {
        std::fprintf(stderr, "  [%zu] %s\n", i, manager.cameras()[i]->id().c_str());
    }

    const bool mono_fallback = left_camera == right_camera;
    CameraEye left(left_camera, eye_width, eye_height, mono_fallback ? "mono" : "left", &notify_cv, &notify_mutex);
    CameraEye right(right_camera, eye_width, eye_height, "right", &notify_cv, &notify_mutex);

    rc = left.open(manager);
    if (rc != 0) {
        manager.stop();
        return 3;
    }
    if (!mono_fallback) {
        rc = right.open(manager);
        if (rc != 0) {
            manager.stop();
            return 4;
        }
        if (left.width() != right.width() || left.height() != right.height()) {
            std::fprintf(stderr, "left/right configured sizes differ\n");
            manager.stop();
            return 5;
        }
    }

    eye_width = left.width();
    eye_height = left.height();
    const size_t rgba_size = static_cast<size_t>(eye_width) * 2u * eye_height * 4u;
    const size_t sbs_yuv_size = static_cast<size_t>(eye_width) * 2u * eye_height * 3u / 2u;
    std::vector<uint8_t> sbs_rgba(rgba_size);
    std::vector<uint8_t> sbs_yuv(use_h264 ? sbs_yuv_size : 0u);
    std::vector<uint8_t> encoded_frame;
    LatestFrame left_frame;
    LatestFrame right_frame;
    left_frame.yuv.resize(left.yuv_size());
    right_frame.yuv.resize(mono_fallback ? left.yuv_size() : right.yuv_size());

    rc = mps_udp_sender_open(&sender, host, port, 3u);
    if (rc != 0) {
        std::fprintf(stderr, "sender open failed: %d\n", rc);
        manager.stop();
        return 6;
    }

#if defined(MPS_ENABLE_GSTREAMER)
    H264Encoder encoder;
    if (use_h264) {
        rc = encoder.init(eye_width * 2u, eye_height, fps, bitrate_kbps, prefer_software_encoder, use_nv12_pack);
        if (rc != 0) {
            std::fprintf(stderr, "H.264 encoder init failed: %d\n", rc);
            manager.stop();
            return 9;
        }
    }
#endif

    rc = left.start();
    if (rc != 0) {
        manager.stop();
        return 7;
    }
    if (!mono_fallback) {
        rc = right.start();
        if (rc != 0) {
            manager.stop();
            return 8;
        }
    }

    std::fprintf(stderr,
                 "streaming libcamera SBS to %s:%u left=%u right=%u eye=%ux%u sbs=%ux%u target_fps=%u max_skew_ms=%llu%s\n",
                 host,
                 port,
                 left_camera,
                 right_camera,
                 eye_width,
                 eye_height,
                 eye_width * 2u,
                 eye_height,
                 fps,
                 static_cast<unsigned long long>(max_skew_ns / 1000000ull),
                 mono_fallback ? " mono_fallback=1" : "");
    if (use_h264) {
        std::fprintf(stderr,
                     "codec=h264 sbs_format=%s bitrate_kbps=%u encoder_mode=%s max_pending=%u max_encode_age_ms=%llu\n",
                     use_nv12_pack ? "nv12" : "i420",
                     bitrate_kbps,
                     encoder_mode.c_str(),
                     max_pending_frames,
                     static_cast<unsigned long long>(max_encode_age_ns / 1000000ull));
    } else {
        std::fprintf(stderr, "codec=raw_rgba\n");
    }

    uint64_t sent_sequence = 0;
    uint64_t encode_sequence = 0;
    uint64_t last_left_sequence = 0;
    uint64_t last_right_sequence = 0;
    uint64_t dropped_skew = 0;
    uint64_t captured_pairs = 0;
    uint64_t encoded_packets = 0;
    uint64_t encoded_bytes = 0;
    uint64_t last_report_ns = now_ns();
    MpsLatencyGate latency_gate{};
    mps_latency_gate_init(&latency_gate, max_pending_frames, max_encode_age_ns);

#if defined(MPS_ENABLE_GSTREAMER)
    auto drain_encoded = [&]() -> int {
        while (encoder.pullFrame(&encoded_frame)) {
            const uint64_t capture_timestamp_ns =
                mps_latency_gate_mark_emitted(&latency_gate, sent_sequence, now_ns());
            const int send_rc = mps_udp_sender_send_frame(&sender,
                                                          encoded_frame.data(),
                                                          encoded_frame.size(),
                                                          sent_sequence,
                                                          capture_timestamp_ns,
                                                          MPS_PACKET_FLAG_KEYFRAME);
            if (send_rc != 0) {
                std::fprintf(stderr, "send failed: %d\n", send_rc);
                return send_rc;
            }
            encoded_packets += 1u;
            encoded_bytes += encoded_frame.size();
            sent_sequence += 1u;
        }
        return 0;
    };
#endif

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(notify_mutex);
            notify_cv.wait_for(lock, std::chrono::milliseconds(100));
        }

#if defined(MPS_ENABLE_GSTREAMER)
        if (use_h264) {
            rc = drain_encoded();
            if (rc != 0) {
                break;
            }
        }
#endif

        if (!left.copyLatest(&left_frame)) {
            continue;
        }
        if (mono_fallback) {
            right_frame = left_frame;
        } else if (!right.copyLatest(&right_frame)) {
            continue;
        }
        if (left_frame.sequence == last_left_sequence && right_frame.sequence == last_right_sequence) {
            continue;
        }

        const uint64_t skew_ns = abs_diff_u64(left_frame.timestamp_ns, right_frame.timestamp_ns);
        if (!mono_fallback && left_frame.timestamp_ns != 0u && right_frame.timestamp_ns != 0u && skew_ns > max_skew_ns) {
            dropped_skew += 1u;
            if ((dropped_skew % 30u) == 1u) {
                std::fprintf(stderr,
                             "dropping unsynced pair skew_ms=%.3f dropped=%llu\n",
                             static_cast<double>(skew_ns) / 1000000.0,
                             static_cast<unsigned long long>(dropped_skew));
            }
            last_left_sequence = left_frame.sequence;
            last_right_sequence = right_frame.sequence;
            continue;
        }
        captured_pairs += 1u;

        if (use_h264) {
#if defined(MPS_ENABLE_GSTREAMER)
            const uint64_t capture_timestamp_ns = left_frame.timestamp_ns ? left_frame.timestamp_ns : now_ns();
            if (!mps_latency_gate_accept(&latency_gate, capture_timestamp_ns, now_ns())) {
                last_left_sequence = left_frame.sequence;
                last_right_sequence = right_frame.sequence;
                continue;
            }

            if (use_nv12_pack) {
                rc = mps_yuv420_pack_sbs_nv12(
                    left_frame.yuv.data(),
                    mono_fallback ? left_frame.yuv.data() : right_frame.yuv.data(),
                    sbs_yuv.data(),
                    eye_width,
                    eye_height);
            } else {
                rc = mono_fallback
                    ? mps_i420_pack_sbs_mono(left_frame.yuv.data(), sbs_yuv.data(), eye_width, eye_height)
                    : mps_i420_pack_sbs(left_frame.yuv.data(), right_frame.yuv.data(), sbs_yuv.data(), eye_width, eye_height);
            }
            if (rc != 0) {
                std::fprintf(stderr, "SBS %s pack failed: %d\n", use_nv12_pack ? "NV12" : "I420", rc);
                break;
            }
            rc = encoder.pushFrame(sbs_yuv.data(), sbs_yuv.size(), encode_sequence);
            if (rc != 0) {
                std::fprintf(stderr, "encoder push failed: %d\n", rc);
                break;
            }
            mps_latency_gate_mark_pushed(&latency_gate, encode_sequence, capture_timestamp_ns);
            encode_sequence += 1u;
            rc = drain_encoded();
            if (rc != 0) {
                break;
            }
#endif
        } else {
            yuv420_eye_to_rgba_sbs(left_frame.yuv.data(), sbs_rgba.data(), eye_width, eye_height, 0u);
            yuv420_eye_to_rgba_sbs(right_frame.yuv.data(), sbs_rgba.data(), eye_width, eye_height, eye_width);

            rc = mps_udp_sender_send_frame(&sender,
                                           sbs_rgba.data(),
                                           sbs_rgba.size(),
                                           sent_sequence,
                                           left_frame.timestamp_ns ? left_frame.timestamp_ns : now_ns(),
                                           MPS_PACKET_FLAG_RAW_RGBA | MPS_PACKET_FLAG_KEYFRAME);
            if (rc != 0) {
                std::fprintf(stderr, "send failed: %d\n", rc);
                break;
            }
            encoded_packets += 1u;
            encoded_bytes += sbs_rgba.size();
            sent_sequence += 1u;
        }
        last_left_sequence = left_frame.sequence;
        last_right_sequence = right_frame.sequence;

        const uint64_t report_ns = now_ns();
        if (report_ns - last_report_ns >= 1000000000ull) {
            std::fprintf(stderr,
                         "stats captured=%llu sent_frames=%llu sent_bytes=%llu udp_packets=%llu pending=%llu drop_pending=%llu drop_age=%llu drop_skew=%llu\n",
                         static_cast<unsigned long long>(captured_pairs),
                         static_cast<unsigned long long>(encoded_packets),
                         static_cast<unsigned long long>(encoded_bytes),
                         static_cast<unsigned long long>(sender.packets_sent),
                         static_cast<unsigned long long>(mps_latency_gate_pending(&latency_gate)),
                         static_cast<unsigned long long>(latency_gate.dropped_pending),
                         static_cast<unsigned long long>(latency_gate.dropped_age),
                         static_cast<unsigned long long>(dropped_skew));
            last_report_ns = report_ns;
        }
    }

    mps_udp_sender_close(&sender);
    manager.stop();
    return rc == 0 ? 0 : rc;
}
