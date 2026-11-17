#include "jetkvm.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>

using json = nlohmann::json;
using namespace std::chrono_literals;
namespace stream {
namespace {
// USB keyboard usages used by the pinned JetKVM keyboardMappings.ts.
constexpr auto kHidMap = std::to_array<std::pair<unsigned, std::string_view>>({{0x04, "KeyA"},
    {0x05, "KeyB"}, {0x06, "KeyC"}, {0x07, "KeyD"}, {0x08, "KeyE"}, {0x09, "KeyF"}, {0x0a, "KeyG"},
    {0x0b, "KeyH"}, {0x0c, "KeyI"}, {0x0d, "KeyJ"}, {0x0e, "KeyK"}, {0x0f, "KeyL"}, {0x10, "KeyM"},
    {0x11, "KeyN"}, {0x12, "KeyO"}, {0x13, "KeyP"}, {0x14, "KeyQ"}, {0x15, "KeyR"}, {0x16, "KeyS"},
    {0x17, "KeyT"}, {0x18, "KeyU"}, {0x19, "KeyV"}, {0x1a, "KeyW"}, {0x1b, "KeyX"}, {0x1c, "KeyY"},
    {0x1d, "KeyZ"}, {0x1e, "Digit1"}, {0x1f, "Digit2"}, {0x20, "Digit3"}, {0x21, "Digit4"},
    {0x22, "Digit5"}, {0x23, "Digit6"}, {0x24, "Digit7"}, {0x25, "Digit8"}, {0x26, "Digit9"},
    {0x27, "Digit0"}, {0x28, "Enter"}, {0x29, "Escape"}, {0x2a, "Backspace"}, {0x2b, "Tab"},
    {0x2c, "Space"}, {0x2d, "Minus"}, {0x2e, "Equal"}, {0x2f, "BracketLeft"},
    {0x30, "BracketRight"}, {0x31, "Backslash"}, {0x33, "Semicolon"}, {0x34, "Quote"},
    {0x35, "Backquote"}, {0x36, "Comma"}, {0x37, "Period"}, {0x38, "Slash"}, {0x39, "CapsLock"},
    {0x3a, "F1"}, {0x3b, "F2"}, {0x3c, "F3"}, {0x3d, "F4"}, {0x3e, "F5"}, {0x3f, "F6"},
    {0x40, "F7"}, {0x41, "F8"}, {0x42, "F9"}, {0x43, "F10"}, {0x44, "F11"}, {0x45, "F12"},
    {0x49, "Insert"}, {0x4a, "Home"}, {0x4b, "PageUp"}, {0x4c, "Delete"}, {0x4d, "End"},
    {0x4e, "PageDown"}, {0x4f, "ArrowRight"}, {0x50, "ArrowLeft"}, {0x51, "ArrowDown"},
    {0x52, "ArrowUp"}, {0x53, "NumLock"}, {0x54, "NumpadDivide"}, {0x55, "NumpadMultiply"},
    {0x56, "NumpadSubtract"}, {0x57, "NumpadAdd"}, {0x58, "NumpadEnter"}, {0x59, "Numpad1"},
    {0x5a, "Numpad2"}, {0x5b, "Numpad3"}, {0x5c, "Numpad4"}, {0x5d, "Numpad5"}, {0x5e, "Numpad6"},
    {0x5f, "Numpad7"}, {0x60, "Numpad8"}, {0x61, "Numpad9"}, {0x62, "Numpad0"},
    {0x63, "NumpadDecimal"}, {0x80, "VolumeUp"}, {0x81, "VolumeDown"}, {0xe0, "ControlLeft"},
    {0xe1, "ShiftLeft"}, {0xe2, "AltLeft"}, {0xe3, "MetaLeft"}, {0xe4, "ControlRight"},
    {0xe5, "ShiftRight"}, {0xe6, "AltRight"}, {0xe7, "MetaRight"}});

struct RpcError : std::runtime_error {
    int code;
    RpcError(int code, const char* message) : std::runtime_error(message), code(code) {}
};
unsigned byte(const json& value) {
    if (!value.is_number_integer() || value < 0 || value > 255) {
        throw std::invalid_argument("Expected a byte");
    }
    return value.get<unsigned>();
}
uint32_t be32(const std::vector<uint8_t>& data, size_t offset) {
    return (static_cast<uint32_t>(data.at(offset)) << 24u) |
           (static_cast<uint32_t>(data.at(offset + 1)) << 16u) |
           (static_cast<uint32_t>(data.at(offset + 2)) << 8u) | data.at(offset + 3);
}
} // namespace
JetKvmProtocol::JetKvmProtocol(
    std::shared_ptr<Input> input, std::string config, std::function<json()> video)
    : input_(std::move(input)), settings_(std::move(config)), video_(std::move(video)),
      macro_worker_([this] { run_macros(); }) {}
JetKvmProtocol::~JetKvmProtocol() {
    stop();
}
void JetKvmProtocol::stop() {
    const std::scoped_lock lock(stop_mutex_);
    if (stopped_) {
        return;
    }
    quit_ = true;
    macro_worker_.request_stop();
    ++generation_;
    {
        const std::scoped_lock queue(macro_mutex_);
        macros_.clear();
    }
    macro_ready_.notify_all();
    macro_worker_.join();
    const std::scoped_lock state(state_mutex_);
    input_->release();
    keys_.clear();
    buttons_ = 0;
    stopped_ = true;
}
void JetKvmProtocol::bind_rpc(SendJson send) {
    const std::scoped_lock lock(state_mutex_);
    if (quit_) {
        return;
    }
    rpc_ = std::move(send);
    rpc_({{"jsonrpc", "2.0"}, {"method", "videoInputState"}, {"params", video_()}});
}
void JetKvmProtocol::bind_hid(SendBinary send) {
    const std::scoped_lock lock(state_mutex_);
    if (quit_) {
        return;
    }
    hid_ = std::move(send);
}
json JetKvmProtocol::down_state_locked() const {
    auto keys = json::array();
    unsigned modifier = 0;
    for (const auto key : keys_) {
        if (key >= 224 && key <= 231) {
            modifier |= 1u << (key - 224);
        } else if (keys.size() < 6) {
            keys.push_back(key);
        }
    }
    while (keys.size() < 6) {
        keys.push_back(0);
    }
    return {{"modifier", modifier}, {"keys", keys}};
}
void JetKvmProtocol::key_state_locked() {
    const auto state = down_state_locked();
    if (hid_) {
        Binary data{0x33, static_cast<uint8_t>(state.at("modifier").get<unsigned>())};
        for (const auto& key : state.at("keys")) {
            data.push_back(key.get<uint8_t>());
        }
        hid_(data);
    }
}
void JetKvmProtocol::key_locked(unsigned usage, bool down) {
    const auto found = std::ranges::find(kHidMap, usage, &decltype(kHidMap)::value_type::first);
    if (found == kHidMap.end()) {
        return;
    }
    const bool was_down = keys_.count(usage) != 0;
    if (down) {
        keys_.insert(usage);
    } else {
        keys_.erase(usage);
    }
    input_->key(found->second, down, down && was_down);
    last_key_ = std::chrono::steady_clock::now();
}
void JetKvmProtocol::keyboard_locked(unsigned modifier, const Binary& keys) {
    if (keys.size() > 6) {
        throw std::invalid_argument("Keyboard report exceeds six keys");
    }
    std::set<unsigned> next;
    for (unsigned i = 0; i < 8; ++i) {
        if (modifier & (1u << i)) {
            next.insert(224 + i);
        }
    }
    for (const auto key : keys) {
        if (key > 3 && std::ranges::find(kHidMap, key, &decltype(kHidMap)::value_type::first) !=
                           kHidMap.end()) {
            next.insert(key);
        }
    }
    const auto previous = keys_;
    for (const auto key : previous) {
        if (!next.count(key)) {
            this->key_locked(key, false);
        }
    }
    for (const auto key : next) {
        if (!keys_.count(key)) {
            this->key_locked(key, true);
        }
    }
    last_key_ = std::chrono::steady_clock::now();
    key_state_locked();
}
void JetKvmProtocol::pointer_locked(double x, double y, unsigned buttons) {
    if (!std::isfinite(x) || !std::isfinite(y) || buttons > 31) {
        throw std::invalid_argument("Invalid pointer report");
    }
    x_ = std::clamp(x, 0.0, 1.0);
    y_ = std::clamp(y, 0.0, 1.0);
    input_->move_pointer(x_, y_);
    buttons_locked(buttons);
}
void JetKvmProtocol::buttons_locked(unsigned buttons) {
    for (const auto& pair : {std::pair<unsigned, Input::Button>{1, Input::Button::Left},
             {2, Input::Button::Right}, {4, Input::Button::Middle}}) {
        if ((buttons_ & pair.first) != (buttons & pair.first)) {
            input_->button(pair.second, static_cast<bool>(buttons & pair.first));
        }
    }
    buttons_ = buttons;
}
void JetKvmProtocol::cancel_macro() {
    ++generation_;
    {
        const std::scoped_lock lock(macro_mutex_);
        // A queued macro already received its start acknowledgement. Complete
        // it here if cancellation wins the race with the worker taking it.
        if (!macros_.empty()) {
            const bool paste = macros_.front().paste;
            macros_.clear();
            macro_running_ = false;
            const std::scoped_lock state(state_mutex_);
            if (hid_) {
                hid_({0x34, 0, static_cast<uint8_t>(paste)});
            }
        }
    }
    macro_ready_.notify_all();
}
void JetKvmProtocol::release() {
    if (quit_) {
        return;
    }
    cancel_macro();
    const std::scoped_lock lock(state_mutex_);
    if (quit_) {
        return;
    }
    input_->release();
    keys_.clear();
    buttons_ = 0;
    key_state_locked();
}
void JetKvmProtocol::tick() {
    const std::scoped_lock lock(state_mutex_);
    if (!macro_running_ && !keys_.empty() && std::chrono::steady_clock::now() - last_key_ > 500ms) {
        keyboard_locked(0, {});
    }
}
json JetKvmProtocol::call(const std::string& method, const json& p) {
    const std::scoped_lock lock(state_mutex_);
    if (quit_) {
        throw RpcError(-32000, "Session closed");
    }
    if (method == "ping") {
        return "pong";
    }
    if (method == "getVideoState") {
        return video_();
    }
    if (method == "getKeyDownState") {
        return down_state_locked();
    }
    if (method == "getKeyboardLedState") {
        return {{"num_lock", false}, {"caps_lock", false}, {"scroll_lock", false},
            {"compose", false}, {"kana", false}};
    }
    if (method == "getAudioConfig") {
        return {{"enabled", false}};
    }
    if (method == "getUsbDevices") {
        return json::object();
    }
    if (method == "getUSBState") {
        return {{"configured", false}};
    }
    if (method == "getKeyboardLayout") {
        return settings_.read()
            .value("console", json::object())
            .value("keyboardLayout", json("en-US"));
    }
    if (method == "getKeyboardMacros") {
        return settings_.read()
            .value("console", json::object())
            .value("keyboardMacros", json::array());
    }
    if (method == "importBrowserPreferences") {
        const auto existing = settings_.read().value("console", json::object());
        auto patch = json::object();
        for (const auto* field : {"keyboardLayout", "keyboardMacros"}) {
            if (!existing.contains(field) && p.contains(field)) {
                patch[field] = p.at(field);
            }
        }
        if (!patch.empty()) {
            settings_.update_console(patch);
        }
        return true;
    }
    if (method == "setKeyboardLayout") {
        settings_.update_console({{"keyboardLayout", p.at("layout")}});
        return true;
    }
    if (method == "setKeyboardMacros") {
        settings_.update_console({{"keyboardMacros", p.at("params").at("macros")}});
        return true;
    }
    if (method == "keypressReport") {
        if (!p.at("press").is_boolean()) {
            throw std::invalid_argument("press must be boolean");
        }
        if (!macro_running_) {
            key_locked(byte(p.at("key")), p.at("press").get<bool>());
            key_state_locked();
        }
        return true;
    }
    if (method == "keyboardReport") {
        if (!p.at("keys").is_array() || p.at("keys").size() > 6) {
            throw std::invalid_argument("Invalid keyboard keys");
        }
        Binary keys;
        for (const auto& value : p.at("keys")) {
            keys.push_back(byte(value));
        }
        if (!macro_running_) {
            keyboard_locked(byte(p.at("modifier")), keys);
        }
        return true;
    }
    if (method == "releaseMouseButtons") {
        buttons_locked(0);
        return true;
    }
    if (method == "absMouseReport") {
        pointer_locked(p.at("x").get<double>() / 32767, p.at("y").get<double>() / 32767,
            byte(p.at("buttons")));
        return true;
    }
    if (method == "relMouseReport") {
        pointer_locked(x_ + p.at("dx").get<double>() / 1920, y_ + p.at("dy").get<double>() / 1080,
            byte(p.at("buttons")));
        return true;
    }
    if (method == "wheelReport") {
        const auto delta = p.at("wheelY").get<double>();
        if (!std::isfinite(delta)) {
            throw std::invalid_argument("Invalid wheel delta");
        }
        input_->wheel(static_cast<int>(std::round(std::clamp(delta, -5.0, 5.0))));
        return true;
    }
    if (method == "tvRemote") {
        const auto code = p.at("code").get<std::string>();
        const std::set<std::string> allowed{"Home", "Escape", "ArrowUp", "ArrowDown", "ArrowLeft",
            "ArrowRight", "Enter", "VolumeUp", "VolumeDown", "AudioVolumeMute", "MediaPlayPause"};
        if (!allowed.count(code)) {
            throw std::invalid_argument("Unknown TV remote key");
        }
        input_->key(code, true);
        input_->key(code, false);
        return true;
    }
    throw RpcError(-32601, "Method unavailable on this TV");
}
void JetKvmProtocol::rpc(const std::string& message) {
    json id = nullptr;
    bool reply = true;
    SendJson send;
    {
        const std::scoped_lock lock(state_mutex_);
        send = rpc_;
    }
    try {
        json request;
        try {
            request = json::parse(message);
        } catch (...) {
            throw RpcError(-32700, "Parse error");
        }
        if (!request.is_object() ||
            (!request.contains("jsonrpc") || request.at("jsonrpc") != "2.0") ||
            !request.contains("method") || !request.at("method").is_string()) {
            throw RpcError(-32600, "Invalid request");
        }
        if (request.contains("id")) {
            const auto& value = request.at("id");
            if (!value.is_null() && !value.is_string() && !value.is_number()) {
                throw RpcError(-32600, "Invalid request ID");
            }
            id = value;
        } else {
            reply = false;
        }
        const auto params = request.value("params", json::object());
        if (!params.is_object()) {
            throw std::invalid_argument("Expected named parameters");
        }
        const auto result = call(request.at("method"), params);
        if (reply && send) {
            send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
        }
    } catch (const RpcError& e) {
        if (reply && send) {
            send({{"jsonrpc", "2.0"}, {"id", id},
                {"error", {{"code", e.code}, {"message", e.what()}}}});
        }
    } catch (const std::exception&) {
        if (reply && send) {
            send({{"jsonrpc", "2.0"}, {"id", id},
                {"error",
                    {{"code", -32602},
                        {"message", "Invalid parameters or unavailable saved preferences"}}}});
        }
    }
}
void JetKvmProtocol::hid(const Binary& d, bool reliable) {
    if (quit_) {
        return;
    }
    if (d.empty() || d.size() > 65536) {
        throw std::invalid_argument("Invalid HID message size");
    }
    const auto type = d[0];
    // Unreliable channels carry movement only. A delayed movement packet must
    // never undo a later reliable button press/release.
    if (!reliable && type != 0x03) {
        throw std::invalid_argument("Unreliable channel accepts pointer movement only");
    }
    if (type == 0x07) {
        if (!reliable || d.size() < 6) {
            throw std::invalid_argument("Invalid macro header");
        }
        const auto count = be32(d, 2);
        if (count > 4096 || d.size() != 6 + static_cast<size_t>(count) * 9) {
            throw std::invalid_argument("Invalid macro length");
        }
        Macro macro{{}, d[1] != 0, generation_};
        unsigned total_delay = 0;
        for (size_t i = 6; i < d.size(); i += 9) {
            Step step{d[i], Binary(d.data() + i + 1, d.data() + i + 7),
                static_cast<unsigned>(d[i + 7]) * 256 + d[i + 8]};
            total_delay += step.delay;
            if (total_delay > 300000) {
                throw std::invalid_argument("Macro exceeds five minutes");
            }
            macro.steps.push_back(std::move(step));
        }
        {
            const std::scoped_lock lock(macro_mutex_);
            if (quit_) {
                return;
            }
            if (macro_running_ || !macros_.empty()) {
                throw std::invalid_argument("A keyboard macro is already running");
            }
            macro_running_ = true;
            macros_.push_back(std::move(macro));
            const std::scoped_lock state(state_mutex_);
            if (hid_) {
                hid_({0x34, 1, static_cast<uint8_t>(d[1] != 0)});
            }
        }
        macro_ready_.notify_all();
        return;
    }
    if (type == 0x08) {
        if (d.size() != 1) {
            throw std::invalid_argument("Invalid cancellation");
        }
        cancel_macro();
        return;
    }
    const std::scoped_lock lock(state_mutex_);
    if (quit_) {
        return;
    }
    switch (type) {
    case 0x01:
        if (d.size() != 2 || d[1] != 1) {
            throw std::invalid_argument("Unsupported HID protocol version");
        }
        if (hid_) {
            hid_({1, 1});
            hid_({0x32, 0});
            key_state_locked();
        }
        break;
    case 0x02:
        if (d.size() < 2 || d.size() > 8) {
            throw std::invalid_argument("Invalid keyboard report");
        }
        if (!macro_running_) {
            keyboard_locked(d[1], Binary(d.begin() + 2, d.end()));
        }
        break;
    case 0x03:
        if (d.size() != 10) {
            throw std::invalid_argument("Invalid pointer report");
        }
        pointer_locked(double(static_cast<int32_t>(be32(d, 1))) / 32767,
            double(static_cast<int32_t>(be32(d, 5))) / 32767, reliable ? d[9] : buttons_);
        break;
    case 0x04:
        if (d.size() != 3) {
            throw std::invalid_argument("Invalid wheel report");
        }
        input_->wheel(static_cast<int>(static_cast<int8_t>(d[1])));
        break;
    case 0x05:
        if (d.size() != 3 || d[2] > 1) {
            throw std::invalid_argument("Invalid keypress report");
        }
        if (!macro_running_) {
            key_locked(d[1], d[2] != 0);
            key_state_locked();
        }
        break;
    case 0x06:
        if (d.size() != 4) {
            throw std::invalid_argument("Invalid relative mouse report");
        }
        pointer_locked(
            x_ + static_cast<int8_t>(d[1]) / 1920.0, y_ + static_cast<int8_t>(d[2]) / 1080.0, d[3]);
        break;
    case 0x09:
        if (d.size() != 1) {
            throw std::invalid_argument("Invalid keepalive");
        }
        last_key_ = std::chrono::steady_clock::now();
        break;
    default:
        throw std::invalid_argument("Unknown HID report");
    }
}
void JetKvmProtocol::run_macros() {
    for (;;) {
        Macro macro;
        {
            std::unique_lock lock(macro_mutex_);
            macro_ready_.wait(lock, [this] { return quit_ || !macros_.empty(); });
            if (quit_) {
                return;
            }
            macro = std::move(macros_.front());
            macros_.pop_front();
            macro_running_ = true;
        }
        auto notify = [&](bool running) {
            const std::scoped_lock lock(state_mutex_);
            if (hid_) {
                hid_({0x34, static_cast<uint8_t>(running), static_cast<uint8_t>(macro.paste)});
            }
        };
        try {
            for (const auto& step : macro.steps) {
                if (quit_ || generation_ != macro.generation) {
                    break;
                }
                {
                    const std::scoped_lock lock(state_mutex_);
                    if (generation_ != macro.generation) {
                        break;
                    }
                    keyboard_locked(step.modifier, step.keys);
                }
                std::unique_lock lock(macro_mutex_);
                if (macro_ready_.wait_for(lock, std::chrono::milliseconds(step.delay),
                        [&] { return quit_ || generation_ != macro.generation; })) {
                    break;
                }
            }
            {
                const std::scoped_lock lock(state_mutex_);
                keyboard_locked(0, {});
            }
            macro_running_ = false;
            notify(false);
        } catch (const std::exception& error) {
            std::cerr << "Macro execution: " << error.what() << '\n';
            input_->release();
        }
        macro_running_ = false;
    }
}
} // namespace stream
