#pragma once

#include "capture_backend.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace stream {
class DynamicLibrary;

inline constexpr std::string_view kRealtekCaptureSource = "Realtek hardware H.264";

// On K24 the HAL extends the encoder callback header with the bounds of the
// mapped bitstream ring. All three addresses are in the process address space.
struct RealtekFrame {
    HardwareFrame header;
    const uint8_t* ring_begin = nullptr;
    const uint8_t* ring_end = nullptr;
};

struct RealtekEncoderApi {
    std::shared_ptr<const DynamicLibrary> library;
    std::function<int()> open, close;
    std::function<int(unsigned, unsigned)> connect;
    std::function<int(unsigned, unsigned, int)> set_param;
    std::function<int(unsigned, HardwareCallback)> register_callback;
    std::function<int(unsigned)> start, stop;
    std::function<int(unsigned, void*, const uint8_t*, uint32_t, const uint8_t*, const uint8_t*)>
        copy_data;
    std::function<int(unsigned, const uint8_t*, uint32_t)> release_data;
};

[[nodiscard]] std::unique_ptr<CaptureHardware> make_realtek_capture_hardware(
    std::shared_ptr<const RealtekEncoderApi> api);
[[nodiscard]] CaptureBackend load_realtek_backend();
} // namespace stream
