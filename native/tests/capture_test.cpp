#include "capture_backend.hpp"
#include "capture_realtek.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

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

CaptureBackend realtek_backend() {
    return {kRealtekCaptureSource, {}};
}

CaptureBackend unavailable_realtek_backend() {
    throw std::runtime_error("Realtek encoder fixture unavailable");
}

void backend_selection(const char* module) {
    const auto nyx = select_capture_backend(module, unavailable_backend, realtek_backend);
    expect(nyx.source == kNyxCaptureSource, "A usable Nyx module must take priority");
    try {
        (void)nyx.open();
        throw std::logic_error("Nyx factory was never called");
    } catch (const std::runtime_error& error) {
        expect(std::string_view(error.what()) == "Nyx fixture selected",
            "Factory must remain callable after the module is selected");
    }
    const std::string missing = std::string(module) + ".missing";
    const auto legacy = select_capture_backend(missing.c_str(), legacy_backend, realtek_backend);
    expect(legacy.source == kLegacyCaptureSource, "Missing Nyx must select the legacy path");
    const auto realtek =
        select_capture_backend(missing.c_str(), unavailable_backend, realtek_backend);
    expect(realtek.source == kRealtekCaptureSource, "Missing Nyx and LG must select Realtek");
    try {
        (void)select_capture_backend(
            missing.c_str(), unavailable_backend, unavailable_realtek_backend);
        throw std::logic_error("Selection succeeded without a working hardware backend");
    } catch (const std::runtime_error& error) {
        const std::string_view message(error.what());
        expect(message.find(missing) != std::string_view::npos &&
                   message.find("LG encoder fixture unavailable") != std::string_view::npos &&
                   message.find("Realtek encoder fixture unavailable") != std::string_view::npos,
            "Selection failure must identify each unavailable backend");
    }
}

void realtek_ring_buffer_capture(const char* lock) {
    enum class Call : uint8_t {
        Open,
        Connect,
        Register,
        Start,
        Copy,
        Release,
        Stop,
        Unregister,
        Close
    };
    std::vector<Call> calls;
    std::vector<std::pair<unsigned, int>> settings;
    HardwareCallback callback = nullptr;
    bool fail_parameter = true;
    bool fail_start = true;
    bool fail_copy = false;
    bool attached_at_stop = false;
    const std::array<uint8_t, 6> ring{0x01, 0x65, 0x11, 0, 0, 0};
    const RealtekFrame frame{
        {0, 2, {}, 9000, ring.data() + 3, 6}, ring.data(), ring.data() + ring.size()};

    auto api = std::make_shared<RealtekEncoderApi>();
    api->open = [&] {
        calls.push_back(Call::Open);
        return 0;
    };
    api->close = [&] {
        calls.push_back(Call::Close);
        return 0;
    };
    api->connect = [&](unsigned port, unsigned input) {
        expect(port == 0 && input == 0, "Realtek encoder must use its direct scaler input");
        calls.push_back(Call::Connect);
        return 0;
    };
    api->set_param = [&](unsigned port, unsigned parameter, int value) {
        expect(port == 0, "Wrong Realtek encoder port");
        settings.emplace_back(parameter, value);
        return fail_parameter ? -1 : 0;
    };
    api->register_callback = [&](unsigned port, HardwareCallback function) {
        expect(port == 0, "Wrong Realtek callback port");
        calls.push_back(function ? Call::Register : Call::Unregister);
        callback = function;
        return 0;
    };
    api->start = [&](unsigned port) {
        expect(port == 0, "Wrong Realtek start port");
        calls.push_back(Call::Start);
        return fail_start ? -1 : 0;
    };
    api->stop = [&](unsigned port) {
        calls.push_back(Call::Stop);
        auto pending = frame;
        attached_at_stop = attached_at_stop || port != 0 ||
                           (callback && callback(&pending.header) == kCaptureSuccess);
        return 0;
    };
    api->copy_data = [&](unsigned port, void* output, const uint8_t* start, uint32_t size,
                         const uint8_t* begin, const uint8_t* end) {
        expect(port == 0 && start == frame.header.base && size == frame.header.bytes &&
                   begin == frame.ring_begin && end == frame.ring_end,
            "Realtek copy must receive the complete ring-buffer span");
        calls.push_back(Call::Copy);
        if (fail_copy) {
            return -1;
        }
        auto* destination = static_cast<std::byte*>(output);
        for (uint32_t index = 0; index < size; ++index) {
            destination[index] =
                static_cast<std::byte>(begin[(start - begin + index) % (end - begin)]);
        }
        return 0;
    };
    api->release_data = [&](unsigned port, const uint8_t* start, uint32_t size) {
        expect(port == 0 && start == frame.header.base && size == frame.header.bytes,
            "Realtek release must advance the hardware ring past the current frame");
        calls.push_back(Call::Release);
        return 0;
    };

    auto capture = make_hardware_capture(
        {kRealtekCaptureSource, [api] { return make_realtek_capture_hardware(api); }}, lock);
    unsigned received = 0;
    const auto receive_frame = [&](const Bytes& bytes, uint32_t timestamp) {
        const Bytes expected{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0x01},
            std::byte{0x65}, std::byte{0x11}};
        expect(bytes == expected && timestamp == 9000, "Realtek Annex B data or timestamp changed");
        ++received;
    };
    constexpr auto cleanup = std::array{Call::Stop, Call::Unregister, Call::Close};
    try {
        capture->start(receive_frame, 8000, resolution("720p"));
        throw std::logic_error("Expected Realtek configuration to fail");
    } catch (const std::runtime_error& error) {
        expect(std::string_view(error.what()).find("Realtek encoder set parameter failed") !=
                   std::string_view::npos,
            "Realtek configuration failure lost its diagnostic");
    }
    expect(calls == std::vector{Call::Open, Call::Connect, Call::Stop, Call::Close},
        "Realtek configuration failure must stop and close the connected encoder");
    fail_parameter = false;
    calls.clear();
    settings.clear();
    try {
        capture->start(receive_frame, 8000, resolution("720p"));
        throw std::logic_error("Expected Realtek start to fail");
    } catch (const std::runtime_error& error) {
        expect(std::string_view(error.what()).find("Realtek encoder start failed") !=
                   std::string_view::npos,
            "Realtek start failure lost its diagnostic");
    }
    expect(calls.size() >= cleanup.size() &&
               std::equal(cleanup.rbegin(), cleanup.rend(), calls.rbegin()) && !attached_at_stop,
        "Realtek start failure must detach callbacks and shut down the encoder");
    expect(std::ranges::find(settings, std::pair{0U, 2}) != settings.end(),
        "Realtek 60 fps must use the firmware's frame rate value");

    fail_start = false;
    calls.clear();
    settings.clear();
    capture->start(receive_frame, 8000, resolution("1080p"));
    expect(std::ranges::find(settings, std::pair{0U, 4}) != settings.end() &&
               std::ranges::find(settings, std::pair{12U, 30}) != settings.end(),
        "Realtek 30 fps must use the firmware's frame rate value and a one-second GOP");
    auto pending = frame;
    expect(callback && callback(&pending.header) == kCaptureSuccess && received == 1,
        "Realtek must deliver an access unit that wraps around the ring");
    expect(
        calls.size() >= 2 && calls[calls.size() - 2] == Call::Copy && calls.back() == Call::Release,
        "Realtek must release the ring buffer after copying");

    fail_copy = true;
    expect(callback(&pending.header) == kCaptureDeviceError && received == 1 &&
               calls.back() == Call::Release,
        "A failed copy must release the buffer and drop the frame");
    capture->stop();
    expect(std::equal(cleanup.rbegin(), cleanup.rend(), calls.rbegin()) && !attached_at_stop &&
               !callback,
        "Realtek stop must detach callbacks and shut down the encoder");
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
    realtek_ring_buffer_capture(lock.c_str());
}
