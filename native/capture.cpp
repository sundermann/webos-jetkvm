#include "capture.hpp"

#include "callback_bridge.hpp"
#include "file.hpp"
#include "nyx.hpp"

#include <condition_variable>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <sys/file.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace stream {
class DisplayFeedback {
    nyx::Api& api_;
    nyx::Plane previous_plane_{};
    int window_ = -1;
    bool screen_open_ = false, plane_changed_ = false;
    bool route_changed_ = false, window_changed_ = false;
    pbnjson::JValue previous_route_, previous_window_;

    static int number(const pbnjson::JValue& state, const char* key) {
        auto value = state[key];
        if (!value.isNumber()) {
            throw std::runtime_error(std::string("Missing Nyx scaler state: ") + key);
        }
        const int result = value.asNumber<int>();
        if (result < 0 || result > 65535) {
            throw std::runtime_error("Invalid Nyx scaler state");
        }
        return result;
    }
    static void rect(pbnjson::JValue& value, const std::string& prefix, const nyx::Rect& region) {
        value.put(prefix + ".x", static_cast<int>(region.x));
        value.put(prefix + ".y", static_cast<int>(region.y));
        value.put(prefix + ".w", static_cast<int>(region.width));
        value.put(prefix + ".h", static_cast<int>(region.height));
    }
    static nyx::Rect saved_rect(const pbnjson::JValue& state, const char* x, const char* y,
        const char* width, const char* height) {
        return {static_cast<uint16_t>(number(state, x)), static_cast<uint16_t>(number(state, y)),
            static_cast<uint16_t>(number(state, width)),
            static_cast<uint16_t>(number(state, height))};
    }
    static pbnjson::JValue region(
        unsigned window, const nyx::Rect& crop, const nyx::Rect& input, const nyx::Rect& output) {
        auto value = pbnjson::Object();
        value.put("wId", static_cast<int>(window));
        rect(value, "inputRegion", crop);
        rect(value, "srcInfo", input);
        rect(value, "outputRegion", output);
        return value;
    }
    template <class Operation> static void restore(const char* name, Operation operation) noexcept {
        try {
            nyx::check(operation(), name);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
        }
    }

  public:
    explicit DisplayFeedback(nyx::Api& api) : api_(api) {}
    DisplayFeedback(const DisplayFeedback&) = delete;
    DisplayFeedback& operator=(const DisplayFeedback&) = delete;
    DisplayFeedback(DisplayFeedback&&) = delete;
    DisplayFeedback& operator=(DisplayFeedback&&) = delete;
    ~DisplayFeedback() {
        if (window_changed_) {
            restore("restore scaler region",
                [&] { return api_.scaler.setParam("WinRegion", previous_window_); });
        }
        if (route_changed_) {
            restore("restore scaler route",
                [&] { return api_.scaler.setParam("Connect", previous_route_); });
        }
        if (plane_changed_) {
            restore("restore capture plane",
                [&] { return api_.screen.setPlaneProperties(previous_plane_); });
        }
        if (window_ >= 0) {
            restore("close scaler",
                [&] { return api_.scaler.close(static_cast<nyx::Window>(window_)); });
        }
        if (screen_open_) {
            restore("close screen capture", [&] { return api_.screen.close(); });
        }
    }
    void configure(const Resolution& resolution) {
        nyx::check(api_.screen.open(),
            "open screen capture (root required; another capture may be active)");
        screen_open_ = true;
        nyx::check(api_.screen.getPlaneProperties(&previous_plane_), "read capture plane");
        if (previous_plane_.buffers || previous_plane_.location) {
            throw std::runtime_error("Display capture is already in use");
        }
        nyx::VideoWindow video{};
        nyx::check(api_.screen.getVideoWindowInfo(&video), "read display dimensions");
        if (!video.panel.width || !video.panel.height) {
            throw std::runtime_error("Nyx did not report display dimensions");
        }
        const nyx::Rect input{0, 0, video.panel.width, video.panel.height};
        const nyx::Rect output{0, 0, static_cast<uint16_t>(resolution.width),
            static_cast<uint16_t>(resolution.height)};

        // Leave the TV's logical display planes alone, including temporarily
        // disconnected ones used by overlays. Capture uses a spare Nyx window.
        nyx::ScalerCapability capability{};
        nyx::check(api_.scaler.getDeviceCapability(&capability), "read scaler capability");
        if (!capability.display_planes) {
            throw std::runtime_error("Nyx did not report display planes");
        }
        for (unsigned candidate = capability.display_planes; candidate < nyx::kScalerWindows;
            ++candidate) {
            int fd = -1;
            if (api_.scaler.open(static_cast<nyx::Window>(candidate), &fd)) {
                continue;
            }
            window_ = static_cast<int>(candidate);
            auto query = pbnjson::Object();
            query.put("wId", static_cast<int>(candidate));
            pbnjson::JValue state;
            if (api_.scaler.getParam("Status", query, &state) == 0 &&
                number(state, "output") == 0 && number(state, "rotation") == 0) {
                previous_route_ = pbnjson::Object();
                previous_route_.put("wId", static_cast<int>(candidate));
                previous_route_.put("type", number(state, "inputSrc"));
                previous_route_.put("resourceIndex", number(state, "inputIndex"));
                previous_route_.put("attr", number(state, "inputAttr"));
                previous_route_.put("outputmode", 0);
                previous_window_ = region(candidate,
                    saved_rect(state, "inputCropX", "inputCropY", "inputCropW", "inputCropH"),
                    saved_rect(state, "inputResX", "inputResY", "inputResW", "inputResH"),
                    saved_rect(state, "outputX", "outputY", "outputW", "outputH"));
                break;
            }
            nyx::check(
                api_.scaler.close(static_cast<nyx::Window>(candidate)), "close occupied scaler");
            window_ = -1;
        }
        if (window_ < 0) {
            throw std::runtime_error("No idle Nyx auxiliary scaler is available");
        }
        const auto next_window = region(static_cast<unsigned>(window_), input, input, output);
        plane_changed_ = true;
        nyx::check(api_.screen.setPlaneProperties({nyx::kFramebufferPlane, output, 0}),
            "configure capture plane");
        route_changed_ = true;
        nyx::check(api_.scaler.setConnectionInfo(static_cast<nyx::Window>(window_),
                       {nyx::kFramebufferInput, 0, 0, {}}, nyx::kEncoderOutput),
            "route display to encoder");
        window_changed_ = true;
        nyx::check(api_.scaler.setParam("WinRegion", next_window), "scale display");
    }
};

// Own a successfully opened encoder even when later configuration fails.
class EncoderSession {
    nyx::Api& api_;
    bool opened_ = false;
    bool started_ = false;

  public:
    explicit EncoderSession(nyx::Api& api) : api_(api) {}
    ~EncoderSession() {
        if (started_ && api_.encoder.stop(0)) {
            std::cerr << "Nyx encoder stop failed\n";
        }
        if (opened_ && api_.encoder.close()) {
            std::cerr << "Nyx encoder close failed\n";
        }
    }
    EncoderSession(const EncoderSession&) = delete;
    EncoderSession& operator=(const EncoderSession&) = delete;
    EncoderSession(EncoderSession&&) = delete;
    EncoderSession& operator=(EncoderSession&&) = delete;
    void configure(unsigned bitrate, const Resolution& resolution) {
        auto& encoder = api_.encoder;
        nyx::check(encoder.open(), "open encoder");
        opened_ = true;
        nyx::check(encoder.connect(0, static_cast<nyx::EncoderInput>(0)), "connect display input");
        using Parameter = nyx::Parameter;
        for (const auto& setting :
            {std::pair{Parameter::Codec, 0}, {Parameter::Width, static_cast<int>(resolution.width)},
                {Parameter::Height, static_cast<int>(resolution.height)},
                {Parameter::FrameRate, resolution.fps == 60 ? 3 : 7}, {Parameter::Profile, 0},
                {Parameter::Level, 3}, {Parameter::Bitrate, static_cast<int>(bitrate)},
                {Parameter::Mode, 0}, {Parameter::GopLength, static_cast<int>(resolution.fps)}}) {
            nyx::check(encoder.setParam(0, static_cast<::nyx::Venc_1_0::ParamType>(setting.first),
                           setting.second),
                "configure encoder");
        }
    }
    void start() {
        // Some driver failures occur after partial startup and still require stop.
        started_ = true;
        nyx::check(api_.encoder.start(0), "start encoder");
    }
};

class HardwareCapture final : public Capture {
    using Bridge = CallbackBridge<nyx_error_t(nyx::Frame*), nyx::kDeviceError>;
    struct Resources {
        UniqueFd lock;
        nyx::Api api;
        DisplayFeedback display{api};
        EncoderSession encoder{api};
        // Destroyed before encoder/display/API: drains every in-flight callback.
        std::unique_ptr<Bridge> callback;
        explicit Resources(UniqueFd descriptor) : lock(std::move(descriptor)) {}
    };
    std::unique_ptr<Resources> resources_;
    // Guards idr_; resource ownership changes only after the control thread joins.
    std::mutex mutex_;
    std::condition_variable_any wake_;
    bool idr_ = false;
    std::jthread control_;

  public:
    HardwareCapture() = default;
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
        UniqueFd lock(
            ::open("/tmp/webos-venc.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (lock.get() < 0 || flock(lock.get(), LOCK_EX | LOCK_NB) < 0) {
            throw std::runtime_error("Hardware encoder is already in use");
        }
        auto resources = std::make_unique<Resources>(std::move(lock));
        resources->display.configure(resolution);
        resources->encoder.configure(bitrate, resolution);
        resources->callback = std::make_unique<Bridge>(
            [api = &resources->api, callback = std::move(callback)](nyx::Frame* value) {
                if (!value || !value->bytes || value->bytes > 4 * 1024 * 1024) {
                    return nyx::kDeviceError;
                }
                Bytes bytes(value->bytes);
                // Nyx supplies a ring-buffer base, not the current access unit.
                if (api->encoder.copyData(value->port, reinterpret_cast<uint8_t*>(bytes.data()),
                        value->base, value->bytes, nullptr, nullptr)) {
                    return nyx::kDeviceError;
                }
                callback(std::move(bytes), static_cast<uint32_t>(value->timestamp));
                return nyx::kSuccess;
            });
        resources->callback->activate();
        nyx::check(resources->api.encoder.registerCallback(0, Bridge::invoke),
            "register encoder callback");
        resources->encoder.start();
        resources_ = std::move(resources);
        try {
            idr_ = false;
            control_ = std::jthread([this](const std::stop_token& stop) {
                std::unique_lock lock(mutex_);
                while (wake_.wait(lock, stop, [this] { return idr_; })) {
                    idr_ = false;
                    lock.unlock();
                    if (resources_->api.encoder.resetGop(0)) {
                        std::cerr << "Nyx keyframe request failed\n";
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
};
std::unique_ptr<Capture> make_capture() {
    return std::make_unique<HardwareCapture>();
}
} // namespace stream
