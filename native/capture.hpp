#pragma once
#include "annexb.hpp"
#include "video.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace stream {
using FrameCallback = std::function<void(Bytes, uint32_t)>;
class Capture {
  public:
    Capture() = default;
    virtual ~Capture() = default;
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    Capture(Capture&&) = delete;
    Capture& operator=(Capture&&) = delete;
    virtual void start(FrameCallback callback, unsigned int bitrate,
        const Resolution& resolution = kResolutions.back()) = 0;
    virtual void keyframe() = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual std::string_view source() const noexcept = 0;
};
[[nodiscard]] std::unique_ptr<Capture> make_capture();
} // namespace stream
