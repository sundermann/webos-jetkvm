#include "capture_backend.hpp"
#include "test_support.hpp"

#include <memory>
#include <stdexcept>
#include <string_view>

namespace {
using namespace stream;

void expect(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

CaptureBackend legacy_backend() {
    return {kLegacyCaptureSource, {}};
}

CaptureBackend unavailable_backend() {
    throw std::runtime_error("LG encoder fixture unavailable");
}

void backend_selection(const char* module) {
    const auto nyx = select_capture_backend(module, unavailable_backend);
    expect(nyx.source == kNyxCaptureSource, "A usable Nyx module must take priority");
    try {
        (void)nyx.open();
        throw std::logic_error("Nyx factory was never called");
    } catch (const std::runtime_error& error) {
        expect(std::string_view(error.what()) == "Nyx fixture selected",
            "Factory must remain callable after the module is selected");
    }
    const std::string missing = std::string(module) + ".missing";
    const auto legacy = select_capture_backend(missing.c_str(), legacy_backend);
    expect(legacy.source == kLegacyCaptureSource, "Missing Nyx must select the legacy path");
    try {
        (void)select_capture_backend(missing.c_str(), unavailable_backend);
        throw std::logic_error("Selection succeeded without a working hardware backend");
    } catch (const std::runtime_error& error) {
        const std::string_view message(error.what());
        expect(message.find(missing) != std::string_view::npos &&
                   message.find("LG encoder fixture unavailable") != std::string_view::npos,
            "Selection failure must identify both unavailable backends");
    }
}

struct DriverState {
    HardwareCallback callback = nullptr;
    bool fail_start = true;
    unsigned destroyed = 0;
    unsigned detached = 0;
    unsigned copied = 0;
};

class Driver final : public CaptureHardware {
    DriverState& state_;

  public:
    explicit Driver(DriverState& state) : state_(state) {}
    ~Driver() override {
        ++state_.destroyed;
        HardwareFrame frame{};
        frame.bytes = 1;
        if (state_.callback && state_.callback(&frame) != kCaptureSuccess) {
            ++state_.detached;
        }
    }
    void configure(unsigned bitrate, const Resolution& selected) override {
        expect(bitrate == 8000 && selected.width == 1280, "Capture settings were lost");
    }
    void register_callback(HardwareCallback callback) override {
        state_.callback = callback;
    }
    void start() override {
        if (state_.fail_start) {
            throw std::runtime_error("Injected encoder start failure");
        }
    }
    bool copy_frame(const HardwareFrame&, std::span<std::byte> destination) override {
        ++state_.copied;
        destination.front() = std::byte{0x65};
        return true;
    }
    bool reset_gop() override {
        return true;
    }
};

void restart_after_failure(const char* lock) {
    DriverState driver;
    auto capture = make_hardware_capture(
        {kLegacyCaptureSource, [&] { return std::make_unique<Driver>(driver); }}, lock);
    unsigned received = 0;
    const auto on_frame = [&](const Bytes& bytes, uint32_t timestamp) {
        expect(bytes == Bytes{std::byte{0x65}} && timestamp == 9000, "Encoded access unit changed");
        ++received;
    };
    try {
        capture->start(on_frame, 8000, resolution("720p"));
        throw std::logic_error("Encoder start should have failed");
    } catch (const std::runtime_error& error) {
        expect(std::string_view(error.what()) == "Injected encoder start failure",
            "Wrong capture start error");
    }
    expect(driver.destroyed == 1 && driver.detached == 1 && !driver.copied,
        "Failed start must detach the callback before releasing hardware");
    driver.fail_start = false;
    capture->start(on_frame, 8000, resolution("720p"));
    HardwareFrame frame{};
    frame.bytes = 1;
    frame.timestamp = 9000;
    expect(driver.callback(&frame) == kCaptureSuccess && received == 1,
        "Capture must restart and deliver frames after a failed start");
    capture->stop();
    expect(driver.destroyed == 2 && driver.detached == 2 && driver.copied == 1,
        "Stopping must detach the callback before releasing hardware");
}
} // namespace

void capture_backend_regressions(const char* module) {
    backend_selection(module);
    const TemporaryDirectory directory;
    const auto lock = directory.path() / "capture.lock";
    restart_after_failure(lock.c_str());
}
