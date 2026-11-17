#pragma once
#include <array>
#include <stdexcept>
#include <string_view>

namespace stream {
struct Resolution {
    const char* id = "";
    unsigned width = 0, height = 0, fps = 0;
};
inline constexpr std::array<Resolution, 4> kResolutions{{{"360p", 640, 360, 60},
    {"540p", 960, 540, 60}, {"720p", 1280, 720, 60}, {"1080p", 1920, 1080, 30}}};
inline const Resolution& resolution(std::string_view id) {
    for (const auto& preset : kResolutions) {
        if (id == preset.id) {
            return preset;
        }
    }
    throw std::invalid_argument("Resolution must be 360p, 540p, 720p, or 1080p");
}
} // namespace stream
