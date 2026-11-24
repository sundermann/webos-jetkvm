#include "capture_realtek.hpp"

#include "dynamic_library.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace stream {
namespace {
constexpr unsigned kEncoderPort = 0;
constexpr unsigned kScalerInput = 0;
constexpr int kRealtekFrameRate60 = 2;
constexpr int kRealtekFrameRate30 = 4;

static_assert(std::is_standard_layout_v<RealtekFrame> && offsetof(RealtekFrame, header) == 0);
static_assert(sizeof(void*) != 4 ||
              (offsetof(HardwareFrame, bytes) == 20 && offsetof(RealtekFrame, ring_begin) == 24 &&
                  offsetof(RealtekFrame, ring_end) == 28));

void check_encoder(int result, const char* operation) {
    if (result) {
        throw std::runtime_error(std::string("Realtek encoder ") + operation + " failed (" +
                                 std::to_string(result) + ")");
    }
}

bool clean_up_encoder(const char* operation, const auto& call) noexcept {
    try {
        check_encoder(call(), operation);
        return true;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    } catch (...) {
        std::cerr << "Realtek encoder " << operation << " failed\n";
    }
    return false;
}

class RealtekCaptureHardware final : public CaptureHardware {
    std::shared_ptr<const RealtekEncoderApi> api_;
    bool opened_ = false;
    bool connected_ = false;
    bool registered_ = false;
    bool start_attempted_ = false;

  public:
    explicit RealtekCaptureHardware(std::shared_ptr<const RealtekEncoderApi> api)
        : api_(std::move(api)) {}
    ~RealtekCaptureHardware() override {
        const bool stopped =
            !connected_ || clean_up_encoder("stop", [&] { return api_->stop(kEncoderPort); });
        // The encoder thread calls the function pointer unconditionally until it stops.
        if (registered_ && (stopped || !start_attempted_)) {
            clean_up_encoder("unregister callback",
                [&] { return api_->register_callback(kEncoderPort, nullptr); });
        }
        if (opened_) {
            clean_up_encoder("close", [&] { return api_->close(); });
        }
    }
    RealtekCaptureHardware(const RealtekCaptureHardware&) = delete;
    RealtekCaptureHardware& operator=(const RealtekCaptureHardware&) = delete;
    RealtekCaptureHardware(RealtekCaptureHardware&&) = delete;
    RealtekCaptureHardware& operator=(RealtekCaptureHardware&&) = delete;

    void configure(unsigned bitrate, const Resolution& resolution) override {
        check_encoder(api_->open(), "open");
        opened_ = true;
        // HAL Connect enables the recording path. HAL Stop disables it even before Start.
        connected_ = true;
        check_encoder(api_->connect(kEncoderPort, kScalerInput), "connect scaler input");
        for (const auto& [parameter, value] : encoder_settings(bitrate, resolution)) {
            const int setting =
                parameter == EncoderParameter::FrameRate
                    ? (resolution.fps == 60 ? kRealtekFrameRate60 : kRealtekFrameRate30)
                    : value;
            check_encoder(api_->set_param(kEncoderPort, static_cast<unsigned>(parameter), setting),
                "set parameter");
        }
    }

    void register_callback(HardwareCallback callback) override {
        registered_ = true;
        check_encoder(api_->register_callback(kEncoderPort, callback), "register callback");
    }

    void start() override {
        start_attempted_ = true;
        check_encoder(api_->start(kEncoderPort), "start");
    }

    [[nodiscard]] bool copy_frame(
        const HardwareFrame& header, std::span<std::byte> destination) override {
        // The HAL passes the first member of the standard-layout Realtek payload.
        const auto& frame = *reinterpret_cast<const RealtekFrame*>(&header);
        const auto begin = std::bit_cast<std::uintptr_t>(frame.ring_begin);
        const auto end = std::bit_cast<std::uintptr_t>(frame.ring_end);
        const auto start = std::bit_cast<std::uintptr_t>(header.base);
        if (!begin || begin >= end || start < begin || start >= end || header.bytes > end - begin ||
            header.bytes != destination.size()) {
            return false;
        }
        const auto copy = api_->copy_data(header.port, destination.data(), header.base,
            header.bytes, frame.ring_begin, frame.ring_end);
        const auto release = api_->release_data(header.port, header.base, header.bytes);
        return copy == 0 && release == 0;
    }

    [[nodiscard]] bool reset_gop() override {
        // K24's reset call is a stub. The configured GOP provides periodic keyframes.
        return true;
    }
};
} // namespace

std::unique_ptr<CaptureHardware> make_realtek_capture_hardware(
    std::shared_ptr<const RealtekEncoderApi> api) {
    return std::make_unique<RealtekCaptureHardware>(std::move(api));
}

CaptureBackend load_realtek_backend() {
    using SessionCall = int (*)();
    using PortCall = int (*)(unsigned);
    const auto library = std::make_shared<DynamicLibrary>("libhal_venc.so.1");
    // This Realtek-specific dependency distinguishes the scaler HAL from other VENC ABIs.
    (void)library->symbol<void (*)()>("KADP_VENC_Connect_VSC");
    const auto api = std::make_shared<RealtekEncoderApi>(RealtekEncoderApi{
        library,
        library->symbol<SessionCall>("HAL_VENC_Open"),
        library->symbol<SessionCall>("HAL_VENC_Close"),
        library->symbol<int (*)(unsigned, unsigned)>("HAL_VENC_Connect"),
        library->symbol<int (*)(unsigned, unsigned, int)>("HAL_VENC_SetParam"),
        library->symbol<int (*)(unsigned, HardwareCallback)>("HAL_VENC_RegisterEncoderCallback"),
        library->symbol<PortCall>("HAL_VENC_Start"),
        library->symbol<PortCall>("HAL_VENC_Stop"),
        library->symbol<int (*)(unsigned, void*, const uint8_t*, uint32_t, const uint8_t*,
            const uint8_t*)>("HAL_VENC_CopyData"),
        library->symbol<int (*)(unsigned, const uint8_t*, uint32_t)>("HAL_VENC_ReleaseData"),
    });
    return {kRealtekCaptureSource, [api] { return make_realtek_capture_hardware(api); }};
}
} // namespace stream
