#include "mps_libcamera_stereo_capture.h"

#include <libcamera/control_ids.h>
#include <libcamera/framebuffer_allocator.h>
#include <libcamera/formats.h>
#include <libcamera/libcamera.h>
#include <libcamera/property_ids.h>

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace {

struct PlaneMapping {
    void* mapping = nullptr;
    const uint8_t* data = nullptr;
    size_t mapping_size = 0;
    size_t data_size = 0;
};

struct BufferMapping {
    std::vector<PlaneMapping> planes;
};

struct LatestFrame {
    std::vector<uint8_t> i420;
    uint64_t timestamp_ns = 0;
    uint64_t camera_sequence = 0;
    bool valid = false;
};

struct CaptureNotify {
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic<uint64_t> generation{0};
    std::atomic<bool> failed{false};
};

class CameraEye {
public:
    CameraEye(uint32_t index, uint32_t width, uint32_t height, uint32_t fps,
              uint32_t buffer_count, const char* label, CaptureNotify* notify)
        : index_(index), width_(width), height_(height), fps_(fps),
          buffer_count_(std::max(2u, buffer_count)), label_(label), notify_(notify)
    {
        for (unsigned int i = 0; i < 3u; ++i) {
            pool_.emplace_back(frame_size());
        }
    }

    ~CameraEye()
    {
        stop();
        unmapBuffers();
    }

    int open(libcamera::CameraManager& manager)
    {
        const auto& cameras = manager.cameras();
        if (index_ >= cameras.size()) {
            std::fprintf(stderr, "%s libcamera index %u out of range (available=%zu)\n",
                         label_, index_, cameras.size());
            return -1;
        }
        camera_ = cameras[index_];
        int rc = camera_->acquire();
        if (rc != 0) {
            std::fprintf(stderr, "%s libcamera acquire failed: %d\n", label_, rc);
            return rc;
        }
        acquired_ = true;

        config_ = camera_->generateConfiguration({libcamera::StreamRole::VideoRecording});
        if (!config_ || config_->empty()) {
            return -2;
        }
        auto& stream_config = config_->at(0);
        stream_config.pixelFormat = libcamera::formats::YUV420;
        stream_config.size = libcamera::Size(width_, height_);
        stream_config.bufferCount = buffer_count_;
        const auto validation = config_->validate();
        if (validation == libcamera::CameraConfiguration::Invalid ||
            stream_config.pixelFormat != libcamera::formats::YUV420 ||
            stream_config.size.width != width_ || stream_config.size.height != height_) {
            std::fprintf(stderr,
                         "%s libcamera cannot supply exact YUV420 %ux%u (got %s %ux%u)\n",
                         label_, width_, height_, stream_config.pixelFormat.toString().c_str(),
                         stream_config.size.width, stream_config.size.height);
            return -3;
        }
        stride_ = stream_config.stride;
        rc = camera_->configure(config_.get());
        if (rc != 0) {
            return rc;
        }
        stream_ = config_->at(0).stream();
        allocator_ = std::make_unique<libcamera::FrameBufferAllocator>(camera_);
        rc = allocator_->allocate(stream_);
        if (rc < 0) {
            return rc;
        }

        for (const auto& buffer : allocator_->buffers(stream_)) {
            if ((rc = mapBuffer(buffer.get())) != 0) {
                return rc;
            }
            std::unique_ptr<libcamera::Request> request = camera_->createRequest();
            if (!request || (rc = request->addBuffer(stream_, buffer.get())) != 0) {
                return rc != 0 ? rc : -4;
            }
            requests_.push_back(std::move(request));
        }

        const std::optional<libcamera::Rectangle> crop =
            camera_->properties().get(libcamera::properties::ScalerCropMaximum);
        if (crop.has_value()) {
            full_crop_ = *crop;
            has_full_crop_ = true;
        }
        camera_->requestCompleted.connect(this, &CameraEye::requestComplete);
        callback_connected_ = true;
        std::fprintf(stderr, "%s direct libcamera: %s %ux%u stride=%u buffers=%zu\n",
                     label_, camera_->id().c_str(), width_, height_, stride_, requests_.size());
        return 0;
    }

    int start(int sync_mode)
    {
        libcamera::ControlList controls;
        const int64_t frame_time_us = std::max<int64_t>(1, 1000000ll / fps_);
        controls.set(libcamera::controls::FrameDurationLimits,
                     libcamera::Span<const int64_t, 2>({frame_time_us, frame_time_us}));
        if (has_full_crop_) {
            controls.set(libcamera::controls::ScalerCrop, full_crop_);
        }
        if (sync_mode != libcamera::controls::rpi::SyncModeOff &&
            camera_->controls().find(&libcamera::controls::rpi::SyncMode) != camera_->controls().end()) {
            controls.set(libcamera::controls::rpi::SyncMode, sync_mode);
        }

        running_.store(true, std::memory_order_release);
        int rc = camera_->start(&controls);
        if (rc != 0) {
            running_.store(false, std::memory_order_release);
            return rc;
        }
        started_ = true;
        for (auto& request : requests_) {
            if ((rc = camera_->queueRequest(request.get())) != 0) {
                notify_->failed.store(true, std::memory_order_release);
                return rc;
            }
        }
        return 0;
    }

    void stop()
    {
        running_.store(false, std::memory_order_release);
        if (camera_ && callback_connected_) {
            camera_->requestCompleted.disconnect(this, &CameraEye::requestComplete);
            callback_connected_ = false;
        }
        if (camera_ && started_) {
            (void)camera_->stop();
            started_ = false;
        }
        if (camera_ && acquired_) {
            camera_->release();
            acquired_ = false;
        }
    }

    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    friend struct DirectCapture;

    size_t frame_size() const
    {
        return static_cast<size_t>(width_) * height_ * 3u / 2u;
    }

    int mapBuffer(libcamera::FrameBuffer* buffer)
    {
        BufferMapping mapping;
        const long page_size = sysconf(_SC_PAGESIZE);
        if (page_size <= 0) {
            return -1;
        }
        for (const auto& plane : buffer->planes()) {
            const off_t mask = static_cast<off_t>(page_size - 1);
            const off_t map_offset = static_cast<off_t>(plane.offset) & ~mask;
            const size_t delta = static_cast<size_t>(static_cast<off_t>(plane.offset) - map_offset);
            const size_t map_size = plane.length + delta;
            void* address = mmap(nullptr, map_size, PROT_READ, MAP_SHARED,
                                 plane.fd.get(), map_offset);
            if (address == MAP_FAILED) {
                std::fprintf(stderr, "%s mmap failed for camera buffer\n", label_);
                return -1;
            }
            mapping.planes.push_back({address,
                                      static_cast<const uint8_t*>(address) + delta,
                                      map_size, plane.length});
        }
        mappings_.emplace(buffer, std::move(mapping));
        return 0;
    }

    void unmapBuffers()
    {
        for (auto& entry : mappings_) {
            for (auto& plane : entry.second.planes) {
                if (plane.mapping && plane.mapping != MAP_FAILED) {
                    munmap(plane.mapping, plane.mapping_size);
                }
            }
        }
        mappings_.clear();
    }

    bool copyI420(libcamera::FrameBuffer* buffer, uint8_t* destination)
    {
        const auto found = mappings_.find(buffer);
        if (found == mappings_.end() || found->second.planes.size() < 3u) {
            return false;
        }
        const auto& planes = found->second.planes;
        const uint32_t y_stride = stride_ != 0u ? stride_ : width_;
        const uint32_t uv_stride = y_stride / 2u;
        const size_t y_required = static_cast<size_t>(y_stride) * height_;
        const size_t uv_required = static_cast<size_t>(uv_stride) * (height_ / 2u);
        if (planes[0].data_size < y_required || planes[1].data_size < uv_required ||
            planes[2].data_size < uv_required) {
            return false;
        }
        uint8_t* y_out = destination;
        uint8_t* u_out = y_out + static_cast<size_t>(width_) * height_;
        uint8_t* v_out = u_out + static_cast<size_t>(width_ / 2u) * (height_ / 2u);
        for (uint32_t row = 0; row < height_; ++row) {
            std::memcpy(y_out + static_cast<size_t>(row) * width_,
                        planes[0].data + static_cast<size_t>(row) * y_stride, width_);
        }
        for (uint32_t row = 0; row < height_ / 2u; ++row) {
            std::memcpy(u_out + static_cast<size_t>(row) * (width_ / 2u),
                        planes[1].data + static_cast<size_t>(row) * uv_stride, width_ / 2u);
            std::memcpy(v_out + static_cast<size_t>(row) * (width_ / 2u),
                        planes[2].data + static_cast<size_t>(row) * uv_stride, width_ / 2u);
        }
        return true;
    }

    void requestComplete(libcamera::Request* request)
    {
        if (!running_.load(std::memory_order_acquire) ||
            request->status() == libcamera::Request::RequestCancelled) {
            return;
        }
        libcamera::FrameBuffer* buffer = request->findBuffer(stream_);
        if (buffer) {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            if (!latest_.valid) {
                if (pool_.empty()) {
                    dropped_.fetch_add(1u, std::memory_order_relaxed);
                } else {
                    latest_.i420 = std::move(pool_.front());
                    pool_.pop_front();
                }
            } else {
                dropped_.fetch_add(1u, std::memory_order_relaxed);
            }
            if (!latest_.i420.empty() && copyI420(buffer, latest_.i420.data())) {
                latest_.timestamp_ns = buffer->metadata().timestamp;
                latest_.camera_sequence += 1u;
                latest_.valid = true;
            } else {
                latest_.valid = false;
            }
        }

        notify_->generation.fetch_add(1u, std::memory_order_release);
        notify_->condition.notify_one();
        request->reuse(libcamera::Request::ReuseBuffers);
        if (running_.load(std::memory_order_acquire) && camera_->queueRequest(request) != 0) {
            notify_->failed.store(true, std::memory_order_release);
            notify_->condition.notify_all();
        }
    }

    uint32_t index_;
    uint32_t width_;
    uint32_t height_;
    uint32_t fps_;
    uint32_t buffer_count_;
    uint32_t stride_ = 0;
    const char* label_;
    CaptureNotify* notify_;
    std::shared_ptr<libcamera::Camera> camera_;
    std::unique_ptr<libcamera::CameraConfiguration> config_;
    libcamera::Stream* stream_ = nullptr;
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
    std::vector<std::unique_ptr<libcamera::Request>> requests_;
    std::unordered_map<libcamera::FrameBuffer*, BufferMapping> mappings_;
    std::mutex frame_mutex_;
    LatestFrame latest_;
    std::deque<std::vector<uint8_t>> pool_;
    std::atomic<uint64_t> dropped_{0};
    std::atomic<bool> running_{false};
    libcamera::Rectangle full_crop_;
    bool has_full_crop_ = false;
    bool started_ = false;
    bool acquired_ = false;
    bool callback_connected_ = false;
};

struct DirectCapture {
    DirectCapture(uint32_t left_camera, uint32_t right_camera,
                  uint32_t width, uint32_t height, uint32_t fps,
                  uint32_t buffer_count, uint64_t max_skew_ns, bool sync)
        : left(left_camera, width, height, fps, buffer_count, "left", &notify),
          right(right_camera, width, height, fps, buffer_count, "right", &notify),
          frame_size(static_cast<size_t>(width) * height * 3u / 2u),
          max_skew_ns(max_skew_ns), software_sync(sync)
    {
    }

    ~DirectCapture()
    {
        running = false;
        release();
        left.stop();
        right.stop();
        if (manager_started) {
            manager.stop();
            manager_started = false;
        }
    }

    static uint64_t difference(uint64_t a, uint64_t b)
    {
        return a > b ? a - b : b - a;
    }

    void recycleLocked(CameraEye& eye, LatestFrame& frame)
    {
        if (!frame.i420.empty()) {
            eye.pool_.push_back(std::move(frame.i420));
        }
        frame = LatestFrame{};
    }

    int tryAcquire(MpsLibcameraStereoPair* out)
    {
        std::scoped_lock lock(left.frame_mutex_, right.frame_mutex_);
        if (!left.latest_.valid || !right.latest_.valid) {
            return 0;
        }
        const uint64_t skew = difference(left.latest_.timestamp_ns,
                                         right.latest_.timestamp_ns);
        if (max_skew_ns != 0u && skew > max_skew_ns) {
            if (left.latest_.timestamp_ns < right.latest_.timestamp_ns) {
                recycleLocked(left, left.latest_);
                left.dropped_.fetch_add(1u, std::memory_order_relaxed);
            } else {
                recycleLocked(right, right.latest_);
                right.dropped_.fetch_add(1u, std::memory_order_relaxed);
            }
            pair_discards.fetch_add(1u, std::memory_order_relaxed);
            return 0;
        }

        held_left = std::move(left.latest_.i420);
        held_right = std::move(right.latest_.i420);
        const uint64_t left_timestamp = left.latest_.timestamp_ns;
        const uint64_t right_timestamp = right.latest_.timestamp_ns;
        left.latest_ = LatestFrame{};
        right.latest_ = LatestFrame{};
        outstanding = true;
        out->left_i420 = held_left.data();
        out->right_i420 = held_right.data();
        out->frame_size = frame_size;
        out->frame_sequence = ++pair_sequence;
        out->capture_timestamp_ns = std::max(left_timestamp, right_timestamp);
        out->timestamp_skew_ns = skew;
        return 1;
    }

    void release()
    {
        std::scoped_lock lock(left.frame_mutex_, right.frame_mutex_);
        if (!outstanding) {
            return;
        }
        left.pool_.push_back(std::move(held_left));
        right.pool_.push_back(std::move(held_right));
        outstanding = false;
    }

    CaptureNotify notify;
    libcamera::CameraManager manager;
    CameraEye left;
    CameraEye right;
    size_t frame_size;
    uint64_t max_skew_ns;
    bool software_sync;
    bool manager_started = false;
    bool running = false;
    bool outstanding = false;
    uint64_t pair_sequence = 0;
    std::vector<uint8_t> held_left;
    std::vector<uint8_t> held_right;
    std::atomic<uint64_t> pair_discards{0};
};

} // namespace

struct MpsLibcameraStereoCapture {
    std::unique_ptr<DirectCapture> capture;
};

extern "C" {

MpsLibcameraStereoCapture* mps_libcamera_stereo_create(
    uint32_t left_camera, uint32_t right_camera, uint32_t width, uint32_t height,
    uint32_t fps, uint32_t buffer_count, uint64_t max_skew_ns, int software_sync)
{
    if (left_camera == right_camera || width == 0u || height == 0u || fps == 0u ||
        (width & 1u) != 0u || (height & 1u) != 0u) {
        return nullptr;
    }
    auto wrapper = std::make_unique<MpsLibcameraStereoCapture>();
    wrapper->capture = std::make_unique<DirectCapture>(
        left_camera, right_camera, width, height, fps, buffer_count,
        max_skew_ns, software_sync != 0);
    DirectCapture& capture = *wrapper->capture;
    if (capture.manager.start() != 0) {
        return nullptr;
    }
    capture.manager_started = true;
    if (capture.left.open(capture.manager) != 0 ||
        capture.right.open(capture.manager) != 0) {
        return nullptr;
    }
    return wrapper.release();
}

int mps_libcamera_stereo_start(MpsLibcameraStereoCapture* wrapper)
{
    if (!wrapper || !wrapper->capture) {
        return -1;
    }
    DirectCapture& capture = *wrapper->capture;
    capture.notify.failed.store(false, std::memory_order_release);
    const int right_sync = capture.software_sync
        ? libcamera::controls::rpi::SyncModeClient : libcamera::controls::rpi::SyncModeOff;
    const int left_sync = capture.software_sync
        ? libcamera::controls::rpi::SyncModeServer : libcamera::controls::rpi::SyncModeOff;
    int rc = capture.right.start(right_sync);
    if (rc == 0) {
        rc = capture.left.start(left_sync);
    }
    capture.running = rc == 0;
    return rc;
}

int mps_libcamera_stereo_acquire_pair(MpsLibcameraStereoCapture* wrapper,
                                      uint32_t timeout_ms,
                                      MpsLibcameraStereoPair* out_pair)
{
    if (!wrapper || !wrapper->capture || !out_pair) {
        return -1;
    }
    DirectCapture& capture = *wrapper->capture;
    if (!capture.running || capture.outstanding) {
        return -2;
    }
    *out_pair = MpsLibcameraStereoPair{};
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    uint64_t generation = capture.notify.generation.load(std::memory_order_acquire);
    for (;;) {
        const int rc = capture.tryAcquire(out_pair);
        if (rc != 0) {
            return rc;
        }
        if (capture.notify.failed.load(std::memory_order_acquire)) {
            return -3;
        }
        std::unique_lock<std::mutex> lock(capture.notify.mutex);
        const bool woke = capture.notify.condition.wait_until(lock, deadline, [&] {
            return capture.notify.failed.load(std::memory_order_acquire) ||
                   capture.notify.generation.load(std::memory_order_acquire) != generation;
        });
        if (!woke) {
            return 0;
        }
        generation = capture.notify.generation.load(std::memory_order_acquire);
    }
}

void mps_libcamera_stereo_release_pair(MpsLibcameraStereoCapture* wrapper)
{
    if (wrapper && wrapper->capture) {
        wrapper->capture->release();
    }
}

uint64_t mps_libcamera_stereo_dropped_frames(const MpsLibcameraStereoCapture* wrapper)
{
    if (!wrapper || !wrapper->capture) {
        return 0;
    }
    const DirectCapture& capture = *wrapper->capture;
    return capture.left.dropped() + capture.right.dropped();
}

void mps_libcamera_stereo_destroy(MpsLibcameraStereoCapture* wrapper)
{
    delete wrapper;
}

} // extern "C"
