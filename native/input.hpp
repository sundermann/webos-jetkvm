#pragma once
#include "input_device.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string_view>
namespace stream {
class Input {
  public:
    enum class Button : std::uint8_t { Left, Middle, Right };

  private:
    std::unique_ptr<InputDevice> device_;
    // Guards device access, pressed keys, pointer position and the event count.
    mutable std::mutex mutex_;
    std::set<unsigned> pressed_;
    unsigned long events_ = 0;
    int pointer_x_ = -1;
    int pointer_y_ = -1;
    void event(unsigned type, unsigned code, int value);
    void release_locked();
    void set_key(unsigned code, bool down, bool repeat);

  public:
    explicit Input(std::unique_ptr<InputDevice> device);
    ~Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    Input(Input&&) = delete;
    Input& operator=(Input&&) = delete;
    void key(std::string_view code, bool down, bool repeat = false);
    void button(Button button, bool down);
    void move_pointer(double x, double y);
    void wheel(int delta);
    void release() noexcept;
    [[nodiscard]] unsigned long events() const;
    [[nodiscard]] static std::span<const unsigned> key_codes() noexcept;
};
} // namespace stream
