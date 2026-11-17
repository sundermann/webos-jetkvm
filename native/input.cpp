#include "input.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>
namespace stream {
// LG maps KEY_PREVIOUS (412) to TV Back. 165 is KEY_PREVIOUSSONG.
static constexpr unsigned kTvBack = 412;
constexpr auto kKeyMap = std::to_array<std::pair<std::string_view, unsigned>>({{"Escape", kTvBack},
    {"BrowserBack", kTvBack}, {"Home", 125}, {"ArrowUp", 103}, {"ArrowDown", 108},
    {"ArrowLeft", 105}, {"ArrowRight", 106}, {"Enter", 28}, {"Space", 57}, {"Backspace", 14},
    {"Tab", 15}, {"ShiftLeft", 42}, {"ShiftRight", 54}, {"ControlLeft", 29}, {"ControlRight", 97},
    {"AltLeft", 56}, {"AltRight", 100}, {"MetaLeft", 125}, {"MetaRight", 126}, {"Minus", 12},
    {"Equal", 13}, {"BracketLeft", 26}, {"BracketRight", 27}, {"Backslash", 43}, {"Semicolon", 39},
    {"Quote", 40}, {"Backquote", 41}, {"Comma", 51}, {"Period", 52}, {"Slash", 53}, {"Digit1", 2},
    {"Digit2", 3}, {"Digit3", 4}, {"Digit4", 5}, {"Digit5", 6}, {"Digit6", 7}, {"Digit7", 8},
    {"Digit8", 9}, {"Digit9", 10}, {"Digit0", 11}, {"KeyQ", 16}, {"KeyW", 17}, {"KeyE", 18},
    {"KeyR", 19}, {"KeyT", 20}, {"KeyY", 21}, {"KeyU", 22}, {"KeyI", 23}, {"KeyO", 24},
    {"KeyP", 25}, {"KeyA", 30}, {"KeyS", 31}, {"KeyD", 32}, {"KeyF", 33}, {"KeyG", 34},
    {"KeyH", 35}, {"KeyJ", 36}, {"KeyK", 37}, {"KeyL", 38}, {"KeyZ", 44}, {"KeyX", 45},
    {"KeyC", 46}, {"KeyV", 47}, {"KeyB", 48}, {"KeyN", 49}, {"KeyM", 50}, {"VolumeUp", 115},
    {"VolumeDown", 114}, {"AudioVolumeMute", 113}, {"MediaPlayPause", 164}, {"CapsLock", 58},
    {"Delete", 111}, {"Insert", 110}, {"End", 107}, {"PageUp", 104}, {"PageDown", 109}, {"F1", 59},
    {"F2", 60}, {"F3", 61}, {"F4", 62}, {"F5", 63}, {"F6", 64}, {"F7", 65}, {"F8", 66}, {"F9", 67},
    {"F10", 68}, {"F11", 87}, {"F12", 88}, {"NumLock", 69}, {"Numpad0", 82}, {"Numpad1", 79},
    {"Numpad2", 80}, {"Numpad3", 81}, {"Numpad4", 75}, {"Numpad5", 76}, {"Numpad6", 77},
    {"Numpad7", 71}, {"Numpad8", 72}, {"Numpad9", 73}, {"NumpadEnter", 96}, {"NumpadAdd", 78},
    {"NumpadSubtract", 74}, {"NumpadMultiply", 55}, {"NumpadDivide", 98}, {"NumpadDecimal", 83}});

Input::Input(std::unique_ptr<InputDevice> device) : device_(std::move(device)) {
    assert(device_);
}
Input::~Input() {
    release();
}
std::span<const unsigned> Input::key_codes() noexcept {
    static constexpr auto codes = [] {
        std::array<unsigned, kKeyMap.size()> values{};
        for (std::size_t index = 0; index < values.size(); ++index) {
            values[index] = kKeyMap[index].second;
        }
        return values;
    }();
    return codes;
}
void Input::event(unsigned type, unsigned code, int value) {
    device_->write_event(type, code, value);
    ++events_;
}
void Input::release_locked() {
    for (auto key : pressed_) {
        event(1, key, 0);
    }
    pressed_.clear();
    event(0, 0, 0);
}
void Input::release() noexcept {
    const std::scoped_lock lock(mutex_);
    try {
        release_locked();
    } catch (const std::exception& error) {
        std::cerr << "Input release: " << error.what() << '\n';
    }
}
unsigned long Input::events() const {
    const std::scoped_lock lock(mutex_);
    return events_;
}
void Input::set_key(unsigned code, bool down, bool repeat) {
    if (down) {
        if (pressed_.insert(code).second) {
            event(1, code, 1);
        } else if (repeat) {
            event(1, code, 2);
        }
    } else if (pressed_.erase(code)) {
        event(1, code, 0);
    }
    event(0, 0, 0);
}
void Input::key(std::string_view code, bool down, bool repeat) {
    const auto found = std::ranges::find(kKeyMap, code, &decltype(kKeyMap)::value_type::first);
    if (found == kKeyMap.end()) {
        return;
    }
    const std::scoped_lock lock(mutex_);
    if (down) {
        device_->hide_cursor();
    }
    set_key(found->second, down, repeat);
}
void Input::button(Button button, bool down) {
    unsigned code = 0;
    switch (button) {
    case Button::Left:
        code = 272;
        break;
    case Button::Middle:
        code = 274;
        break;
    case Button::Right:
        code = kTvBack;
        break;
    }
    const std::scoped_lock lock(mutex_);
    if (down && button == Button::Right) {
        device_->hide_cursor(); // Right-click is the TV remote's Back key.
    }
    set_key(code, down, false);
}
void Input::move_pointer(double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        throw std::invalid_argument("Invalid pointer coordinates");
    }
    const std::scoped_lock lock(mutex_);
    const int native_x = static_cast<int>(std::clamp(x, 0.0, 1.0) * 1919);
    const int native_y = static_cast<int>(std::clamp(y, 0.0, 1.0) * 1079);
    if (native_x == pointer_x_ && native_y == pointer_y_) {
        return;
    }
    pointer_x_ = native_x;
    pointer_y_ = native_y;
    event(3, 0, native_x);
    event(3, 1, native_y);
    event(0, 0, 0);
}
void Input::wheel(int delta) {
    const std::scoped_lock lock(mutex_);
    if (delta == 0) {
        return;
    }
    event(2, 8, std::clamp(delta, -5, 5));
    event(0, 0, 0);
}
} // namespace stream
