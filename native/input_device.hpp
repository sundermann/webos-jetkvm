#pragma once

#include <memory>

namespace stream {
// Input translates protocol state into device events. Only webOS supplies a
// runtime implementation; unit tests supply their own observer in test code.
class InputDevice {
  public:
    InputDevice() = default;
    virtual ~InputDevice() = default;
    InputDevice(const InputDevice&) = delete;
    InputDevice& operator=(const InputDevice&) = delete;
    InputDevice(InputDevice&&) = delete;
    InputDevice& operator=(InputDevice&&) = delete;
    virtual void write_event(unsigned type, unsigned code, int value) = 0;
    virtual void hide_cursor() = 0;
};
[[nodiscard]] std::unique_ptr<InputDevice> make_webos_input_device();
} // namespace stream
