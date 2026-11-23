#include "callback_bridge.hpp"
#include "capture_backend.hpp"
#include "dynamic_library.hpp"
#include "file.hpp"

#include <condition_variable>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace stream {
namespace {
class HardwareCapture final : public Capture {
    using Bridge = CallbackBridge<nyx_error_t(HardwareFrame*), kCaptureDeviceError>;
    struct Resources {
        UniqueFd lock;
        std::unique_ptr<CaptureHardware> hardware;
        // Detach and drain callbacks before hardware stop and display restoration.
        std::unique_ptr<Bridge> callback;
        explicit Resources(UniqueFd descriptor) : lock(std::move(descriptor)) {}
    };
    CaptureBackend backend_;
    std::string lock_path_;
    std::unique_ptr<Resources> resources_;
    // Guards idr_; resource ownership changes only after the control thread joins.
    std::mutex mutex_;
    std::condition_variable_any wake_;
    bool idr_ = false;
    std::jthread control_;

  public:
    HardwareCapture(CaptureBackend backend, const char* lock_path)
        : backend_(std::move(backend)), lock_path_(lock_path) {}
    ~HardwareCapture() override {
        stop();
    }
    HardwareCapture(const HardwareCapture&) = delete;
    HardwareCapture& operator=(const HardwareCapture&) = delete;
    HardwareCapture(HardwareCapture&&) = delete;
    HardwareCapture& operator=(HardwareCapture&&) = delete;
    void start(FrameCallback callback, unsigned bitrate, const Resolution& resolution) override {
        if (resources_) {
            throw std::logic_error("Hardware capture is already running");
        }
        UniqueFd descriptor(
            ::open(lock_path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (descriptor.get() < 0 || flock(descriptor.get(), LOCK_EX | LOCK_NB) < 0) {
            throw std::runtime_error("Hardware encoder is already in use");
        }
        auto resources = std::make_unique<Resources>(std::move(descriptor));
        resources->hardware = backend_.open();
        resources->hardware->configure(bitrate, resolution);
        resources->callback =
            std::make_unique<Bridge>([hardware = resources->hardware.get(),
                                         callback = std::move(callback)](HardwareFrame* value) {
                if (!value || !value->bytes || value->bytes > 4 * 1024 * 1024) {
                    return kCaptureDeviceError;
                }
                Bytes bytes(value->bytes);
                if (!hardware->copy_frame(*value, bytes)) {
                    return kCaptureDeviceError;
                }
                callback(std::move(bytes), static_cast<uint32_t>(value->timestamp));
                return kCaptureSuccess;
            });
        resources->callback->activate();
        resources->hardware->register_callback(Bridge::invoke);
        resources->hardware->start();
        resources_ = std::move(resources);
        try {
            {
                const std::scoped_lock lock(mutex_);
                idr_ = false;
            }
            control_ = std::jthread([this](const std::stop_token& stop) {
                std::unique_lock lock(mutex_);
                while (wake_.wait(lock, stop, [this] { return idr_; })) {
                    idr_ = false;
                    lock.unlock();
                    if (!resources_->hardware->reset_gop()) {
                        std::cerr << backend_.source << " keyframe request failed\n";
                    }
                    lock.lock();
                }
            });
        } catch (...) {
            resources_.reset();
            throw;
        }
    }
    void keyframe() override {
        {
            const std::scoped_lock lock(mutex_);
            idr_ = true;
        }
        wake_.notify_one();
    }
    void stop() noexcept override {
        control_.request_stop();
        wake_.notify_all();
        if (control_.joinable()) {
            control_.join();
        }
        resources_.reset();
    }
    [[nodiscard]] std::string_view source() const noexcept override {
        return backend_.source;
    }
};

CaptureBackend load_nyx(const char* module) {
    using Factory = CaptureHardware* (*)();
    const auto library = std::make_shared<DynamicLibrary>(module);
    const auto create = library->symbol<Factory>("create_nyx_capture_backend");
    return {kNyxCaptureSource,
        [library, create] { return std::unique_ptr<CaptureHardware>(create()); }};
}
} // namespace

CaptureBackend select_capture_backend(const char* nyx_module, CaptureBackend (*legacy_backend)()) {
    try {
        return load_nyx(nyx_module);
    } catch (const std::runtime_error& nyx_error) {
        try {
            return legacy_backend();
        } catch (const std::runtime_error& legacy_error) {
            throw std::runtime_error(std::string("No working hardware capture backend. Nyx: ") +
                                     nyx_error.what() + "; LG encoder: " + legacy_error.what());
        }
    }
}

std::unique_ptr<Capture> make_hardware_capture(CaptureBackend backend, const char* lock_path) {
    return std::make_unique<HardwareCapture>(std::move(backend), lock_path);
}
} // namespace stream
