#pragma once

#include "capture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

enum nyx_error_t : int32_t;

namespace nyx::Venc_1_0 {
// Prefix shared by the Nyx and O22 HAL hardware encoder callbacks.
struct FrameInfo {
    uint32_t port{};
    uint8_t type = 0;
    std::array<uint8_t, 3> padding{};
    uint64_t timestamp{};
    const uint8_t* base = nullptr;
    uint32_t bytes{};
};
} // namespace nyx::Venc_1_0

namespace stream {
using HardwareFrame = ::nyx::Venc_1_0::FrameInfo;
using HardwareCallback = nyx_error_t (*)(HardwareFrame*);
constexpr auto kCaptureSuccess = static_cast<nyx_error_t>(0);
constexpr auto kCaptureDeviceError = static_cast<nyx_error_t>(5);
constexpr std::string_view kNyxCaptureSource = "Nyx hardware H.264";
constexpr std::string_view kLegacyCaptureSource = "LG hardware H.264";

enum class EncoderParameter : uint8_t {
    FrameRate = 0,
    Width = 1,
    Height = 2,
    Bitrate = 7,
    Profile = 8,
    Level = 9,
    Codec = 10,
    Mode = 11,
    GopLength = 12
};

[[nodiscard]] inline auto encoder_settings(unsigned bitrate, const Resolution& resolution) {
    using Parameter = EncoderParameter;
    return std::array<std::pair<Parameter, int>, 9>{
        {{Parameter::Codec, 0}, {Parameter::Width, static_cast<int>(resolution.width)},
            {Parameter::Height, static_cast<int>(resolution.height)},
            {Parameter::FrameRate, resolution.fps == 60 ? 3 : 7}, {Parameter::Profile, 0},
            {Parameter::Level, 3}, {Parameter::Bitrate, static_cast<int>(bitrate)},
            {Parameter::Mode, 0}, {Parameter::GopLength, static_cast<int>(resolution.fps)}}};
}

class CaptureHardware {
  public:
    virtual ~CaptureHardware() = default;
    virtual void configure(unsigned bitrate, const Resolution& resolution) = 0;
    virtual void register_callback(HardwareCallback callback) = 0;
    virtual void start() = 0;
    [[nodiscard]] virtual bool copy_frame(
        const HardwareFrame& frame, std::span<std::byte> destination) = 0;
    [[nodiscard]] virtual bool reset_gop() = 0;
};

struct CaptureBackend {
    std::string_view source;
    std::function<std::unique_ptr<CaptureHardware>()> open;
};

[[nodiscard]] CaptureBackend select_capture_backend(
    const char* nyx_module, CaptureBackend (*legacy_backend)());
[[nodiscard]] std::unique_ptr<Capture> make_hardware_capture(
    CaptureBackend backend, const char* lock_path = "/tmp/webos-venc.lock");
} // namespace stream
