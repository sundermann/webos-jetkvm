#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <pbnjson.hpp>
#include <stdexcept>
#include <string>

// Existing retail TV ARM32 ABI declarations, now bound by the native linker.
// Proxy objects are owned by Nyx. Only references cross this interface; their
// implementation, object layout, and module selection remain in the TV library.
enum nyx_error_t : int32_t;

namespace nyx {
namespace ScreenCapture_1_0 {
struct Rect {
    uint16_t x = 0, y = 0, width = 0, height = 0;
};
struct PlaneProperties {
    uint32_t location{};
    Rect rect{};
    int32_t buffers{};
};
struct VideoWindowInfo {
    uint32_t source{};
    Rect input{}, output{}, panel{};
};
} // namespace ScreenCapture_1_0

namespace Venc_1_0 {
struct FrameInfo {
    uint32_t port{};
    uint8_t type = 0;
    std::array<uint8_t, 3> padding{};
    uint64_t timestamp{};
    const uint8_t* base = nullptr;
    uint32_t bytes{};
};
using EncoderCallback = nyx_error_t (*)(FrameInfo*);

// Opaque foreign ABI enum. Project parameter IDs are declared separately below.
enum class ParamType : uint32_t;
enum class InputType : uint32_t;
} // namespace Venc_1_0

namespace Vscaler_1_0 {
struct InputInfo {
    uint32_t source{};
    uint8_t attribute = 0, index = 0;
    std::array<uint8_t, 2> padding{};
};
struct DeviceCapability {
    uint32_t display_planes{};
};
enum class WindowId : uint32_t;
enum class OutputMode : uint32_t;
} // namespace Vscaler_1_0

class VencProxy {
  public:
    VencProxy() = delete;
    nyx_error_t open();
    nyx_error_t close();
    nyx_error_t connect(unsigned port, Venc_1_0::InputType input);
    nyx_error_t setParam(unsigned port, Venc_1_0::ParamType parameter, int value);
    nyx_error_t registerCallback(unsigned port, Venc_1_0::EncoderCallback callback);
    nyx_error_t copyData(unsigned port, uint8_t* destination, const uint8_t* source, unsigned size,
        const uint8_t* start, const uint8_t* end);
    nyx_error_t start(unsigned port);
    nyx_error_t stop(unsigned port);
    nyx_error_t resetGop(unsigned port);
};

class ScreenCaptureProxy {
  public:
    ScreenCaptureProxy() = delete;
    nyx_error_t open();
    nyx_error_t close();
    nyx_error_t getPlaneProperties(ScreenCapture_1_0::PlaneProperties* plane);
    nyx_error_t setPlaneProperties(const ScreenCapture_1_0::PlaneProperties& plane);
    nyx_error_t getVideoWindowInfo(ScreenCapture_1_0::VideoWindowInfo* window);
};

class VscalerProxy {
  public:
    VscalerProxy() = delete;
    nyx_error_t getDeviceCapability(Vscaler_1_0::DeviceCapability* capability);
    nyx_error_t open(Vscaler_1_0::WindowId window, int* fd);
    nyx_error_t close(Vscaler_1_0::WindowId window);
    nyx_error_t getParam(
        const std::string& control, const pbnjson::JValue& parameters, pbnjson::JValue* output);
    nyx_error_t setParam(const std::string& control, const pbnjson::JValue& parameters);
    nyx_error_t setConnectionInfo(Vscaler_1_0::WindowId window, const Vscaler_1_0::InputInfo& input,
        Vscaler_1_0::OutputMode output);
};

// The firmware getters return references to the corresponding proxy singletons.
VencProxy& getVenc();
ScreenCaptureProxy& getScreenCapture();
VscalerProxy& getVscaler();
} // namespace nyx

namespace stream::nyx {
using Rect = ::nyx::ScreenCapture_1_0::Rect;
using Plane = ::nyx::ScreenCapture_1_0::PlaneProperties;
using VideoWindow = ::nyx::ScreenCapture_1_0::VideoWindowInfo;
using Input = ::nyx::Vscaler_1_0::InputInfo;
using ScalerCapability = ::nyx::Vscaler_1_0::DeviceCapability;
using Frame = ::nyx::Venc_1_0::FrameInfo;
enum class Parameter : uint8_t {
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
using EncoderInput = ::nyx::Venc_1_0::InputType;
using Window = ::nyx::Vscaler_1_0::WindowId;

static_assert(sizeof(Plane) == 16 && sizeof(Input) == 8 && sizeof(VideoWindow) == 28);
static_assert(sizeof(nyx_error_t) == 4);
static_assert(sizeof(void*) == 4 && offsetof(Frame, bytes) == 20, "Nyx TV ABI requires ARM32");

constexpr uint32_t kFramebufferPlane = 3;
constexpr uint32_t kFramebufferInput = 8;
constexpr auto kEncoderOutput = static_cast<::nyx::Vscaler_1_0::OutputMode>(2);
constexpr unsigned kScalerWindows = 4; // MAIN, SUB0, SUB1, SUB2 in Vscaler 1.0.
constexpr auto kSuccess = static_cast<nyx_error_t>(0);
constexpr auto kDeviceError = static_cast<nyx_error_t>(5);

struct Api {
    ::nyx::VencProxy& encoder = ::nyx::getVenc();
    ::nyx::ScreenCaptureProxy& screen = ::nyx::getScreenCapture();
    ::nyx::VscalerProxy& scaler = ::nyx::getVscaler();
};

inline void check(int result, const char* operation) {
    if (result) {
        throw std::runtime_error(
            std::string("Nyx ") + operation + " failed (" + std::to_string(result) + ")");
    }
}
} // namespace stream::nyx
