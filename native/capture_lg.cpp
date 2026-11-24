#include "capture_backend.hpp"
#include "capture_realtek.hpp"
#include "dynamic_library.hpp"
#include "file.hpp"

#include <array>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <linux/videodev2.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <utility>

namespace stream {
namespace {
constexpr uint32_t kWindowInfo = 0x00982a02;
constexpr uint32_t kCapturePlane = 0x00982a03;
constexpr uint32_t kScalerConnection = 0x0098290c;
constexpr uint32_t kScalerRegion = 0x00982900;
constexpr uint32_t kFramebufferPlane = 3;
constexpr uint32_t kFramebufferSource = 6;
constexpr uint32_t kEncoderOutput = 2;

struct Rect {
    uint16_t x{}, y{}, width{}, height{};
};
struct Plane {
    uint32_t location{};
    Rect rect{};
    int32_t buffers{};
};
struct Connection {
    uint32_t source{};
    uint8_t index{}, attribute{};
    std::array<uint8_t, 2> padding{};
    uint32_t output{};
};
struct WindowInfo {
    uint32_t source{};
    Rect input{}, output{}, panel{};
};
struct Region {
    Rect crop{}, input{}, output{};
    uint32_t rotation{};
    Rect punch{};
};
static_assert(sizeof(Plane) == 16 && sizeof(Connection) == 12 && sizeof(WindowInfo) == 28);
static_assert(sizeof(Region) == 36 && offsetof(Region, punch) == 28);
static_assert(
    sizeof(void*) == 4 && offsetof(HardwareFrame, bytes) == 20, "LG TV HAL requires ARM32");

template <class Payload>
bool transfer(int fd, unsigned long request, uint32_t id, uint32_t& size, Payload& payload) {
    v4l2_ext_control control{};
    control.id = id;
    control.size = size;
    // Older SDK headers name the payload pointer in this union string.
    control.string = reinterpret_cast<char*>(&payload);
    v4l2_ext_controls controls{};
    controls.ctrl_class = V4L2_CTRL_CLASS_USER;
    controls.count = 1;
    controls.controls = &control;
    const auto result = ioctl(fd, request, &controls);
    size = control.size;
    return result == 0;
}

template <class Payload> class SavedControl {
    uint32_t id_;
    const char* name_;
    int fd_ = -1;
    Payload saved_{};
    uint32_t size_ = sizeof(Payload);
    bool changed_ = false;

  public:
    explicit SavedControl(uint32_t id, const char* name) : id_(id), name_(name) {}
    ~SavedControl() {
        auto size = size_;
        if (changed_ && !transfer(fd_, VIDIOC_S_EXT_CTRLS, id_, size, saved_)) {
            std::cerr << "Could not restore LG " << name_ << '\n';
        }
    }
    SavedControl(const SavedControl&) = delete;
    SavedControl& operator=(const SavedControl&) = delete;
    SavedControl(SavedControl&&) = delete;
    SavedControl& operator=(SavedControl&&) = delete;

    [[nodiscard]] bool read(int fd, uint32_t minimum_size = sizeof(Payload)) {
        fd_ = fd;
        saved_ = {};
        size_ = sizeof(Payload);
        return transfer(fd, VIDIOC_G_EXT_CTRLS, id_, size_, saved_) &&
               (size_ == minimum_size || size_ == sizeof(Payload));
    }
    [[nodiscard]] const Payload& value() const noexcept {
        return saved_;
    }
    void write(Payload value) {
        changed_ = true;
        auto size = size_;
        if (!transfer(fd_, VIDIOC_S_EXT_CTRLS, id_, size, value)) {
            throw std::runtime_error(std::string("Could not configure LG ") + name_);
        }
    }
};

class DisplayFeedback {
    UniqueFd capture_, scaler_;
    SavedControl<Plane> plane_{kCapturePlane, "capture plane"};
    SavedControl<Connection> connection_{kScalerConnection, "scaler connection"};
    SavedControl<Region> region_{kScalerRegion, "scaler region"};

  public:
    void configure(const Resolution& resolution) {
        capture_ = UniqueFd(open("/dev/video60", O_RDWR | O_NONBLOCK | O_CLOEXEC));
        if (capture_.get() < 0 || !plane_.read(capture_.get())) {
            throw std::runtime_error("Could not read LG display capture plane");
        }
        if (plane_.value().location || plane_.value().buffers) {
            throw std::runtime_error("Display capture is already in use");
        }

        WindowInfo window{};
        uint32_t size = sizeof(window);
        if (!transfer(capture_.get(), VIDIOC_G_EXT_CTRLS, kWindowInfo, size, window) ||
            size != sizeof(window) || !window.panel.width || !window.panel.height) {
            throw std::runtime_error("Could not read LG display dimensions");
        }
        // On O22, video30 and video31 can be owned by the main/overlay displays.
        for (const auto* path : {"/dev/video32", "/dev/video33"}) {
            UniqueFd descriptor(open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC));
            if (descriptor.get() >= 0 && connection_.read(descriptor.get()) &&
                !connection_.value().output &&
                region_.read(descriptor.get(), offsetof(Region, punch)) &&
                !region_.value().rotation) {
                scaler_ = std::move(descriptor);
                break;
            }
        }
        if (scaler_.get() < 0) {
            throw std::runtime_error("No idle LG auxiliary scaler is available");
        }
        const Rect input{0, 0, window.panel.width, window.panel.height};
        const Rect output{0, 0, static_cast<uint16_t>(resolution.width),
            static_cast<uint16_t>(resolution.height)};
        plane_.write({kFramebufferPlane, output, 0});
        connection_.write({kFramebufferSource, 0, 0, {}, kEncoderOutput});
        region_.write({input, input, output, 0, {}});
    }
};

void check_encoder(int result, const char* operation) {
    if (result) {
        throw std::runtime_error(
            std::string("LG encoder ") + operation + " failed (" + std::to_string(result) + ")");
    }
}

class EncoderApi {
    using SessionCall = int (*)();
    using PortCall = int (*)(unsigned);
    DynamicLibrary library_{"libhal_lg115x.so.2"};

  public:
    const SessionCall open = library_.symbol<SessionCall>("HAL_VENC_Open");
    const SessionCall close = library_.symbol<SessionCall>("HAL_VENC_Close");
    const PortCall start = library_.symbol<PortCall>("HAL_VENC_Start");
    const PortCall stop = library_.symbol<PortCall>("HAL_VENC_Stop");
    const PortCall reset_gop = library_.symbol<PortCall>("HAL_VENC_ResetGOP");
    int (*const connect)(unsigned, unsigned) = library_.symbol<decltype(connect)>(
        "HAL_VENC_Connect");
    int (*const set_param)(unsigned, unsigned, int) = library_.symbol<decltype(set_param)>(
        "HAL_VENC_SetParam");
    int (*const register_callback)(unsigned, HardwareCallback) =
        library_.symbol<decltype(register_callback)>("HAL_VENC_RegisterEncoderCallback");
    int (*const copy_data)(unsigned, void*, const void*,
        unsigned) = library_.symbol<decltype(copy_data)>("HAL_VENC_CopyData");
};

class LgCaptureHardware final : public CaptureHardware {
    const EncoderApi& api_;
    DisplayFeedback display_;
    bool opened_ = false;
    bool started_ = false;

  public:
    explicit LgCaptureHardware(const EncoderApi& api) : api_(api) {}
    ~LgCaptureHardware() override {
        if (started_ && api_.stop(0)) {
            std::cerr << "LG encoder stop failed\n";
        }
        if (opened_ && api_.close()) {
            std::cerr << "LG encoder close failed\n";
        }
    }
    LgCaptureHardware(const LgCaptureHardware&) = delete;
    LgCaptureHardware& operator=(const LgCaptureHardware&) = delete;
    LgCaptureHardware(LgCaptureHardware&&) = delete;
    LgCaptureHardware& operator=(LgCaptureHardware&&) = delete;

    void configure(unsigned bitrate, const Resolution& resolution) override {
        display_.configure(resolution);
        check_encoder(api_.open(), "open");
        opened_ = true;
        check_encoder(api_.connect(0, 0), "connect display input");
        for (const auto& [parameter, value] : encoder_settings(bitrate, resolution)) {
            check_encoder(
                api_.set_param(0, static_cast<unsigned>(parameter), value), "set parameter");
        }
    }
    void register_callback(HardwareCallback callback) override {
        check_encoder(api_.register_callback(0, callback), "register callback");
    }
    void start() override {
        started_ = true;
        check_encoder(api_.start(0), "start");
    }
    [[nodiscard]] bool copy_frame(
        const HardwareFrame& frame, std::span<std::byte> destination) override {
        // The HAL needs to copy the current access unit out of its ring buffer.
        return api_.copy_data(frame.port, destination.data(), frame.base, frame.bytes) == 0;
    }
    [[nodiscard]] bool reset_gop() override {
        return api_.reset_gop(0) == 0;
    }
};

CaptureBackend load_lg_backend() {
    std::ifstream device("/var/run/nyx/device_info.json");
    if (!device) {
        throw std::runtime_error("Could not read the TV board type for legacy capture");
    }
    const auto info = nlohmann::json::parse(device, nullptr, false);
    const auto board = info.is_object() ? info.find("board_type") : info.end();
    if (board == info.end() || !board->is_string() ||
        !std::string_view(board->get_ref<const std::string&>()).starts_with("O22_")) {
        throw std::runtime_error("The legacy LG capture backend requires an O22 board");
    }
    const auto api = std::make_shared<EncoderApi>();
    return {kLegacyCaptureSource, [api] { return std::make_unique<LgCaptureHardware>(*api); }};
}
} // namespace

std::unique_ptr<Capture> make_capture() {
    return make_hardware_capture(
        select_capture_backend("libwebos-jetkvm-nyx.so", load_lg_backend, load_realtek_backend));
}
} // namespace stream
