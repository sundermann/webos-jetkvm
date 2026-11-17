#include "jetkvm.hpp"
#include "recording_input.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <iostream>
#include <unistd.h>
using json = nlohmann::json;
using namespace std::chrono_literals;
static void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
int main() try {
    const TemporaryDirectory directory;
    const auto config = (directory.path() / "config.json").string();
    {
        stream::SettingsStore saved(config);
        saved.update(
            {{"password", "fixture-password"}, {"autostart", true}, {"resolution", "720p"}});
        auto device = std::make_unique<RecordingInput>();
        const auto* recorded = device.get();
        auto input = std::make_shared<stream::Input>(std::move(device));
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<json> replies;
        std::vector<stream::JetKvmProtocol::Binary> messages;
        stream::JetKvmProtocol protocol(input, config, [] {
            return json{
                {"ready", true}, {"width", 1280}, {"height", 720}, {"fps", 60}, {"streaming", 1}};
        });
        protocol.bind_rpc([&](const json& j) {
            const std::scoped_lock lock(mutex);
            replies.push_back(j);
        });
        protocol.bind_hid([&](const auto& data) {
            const std::scoped_lock lock(mutex);
            messages.push_back(data);
            changed.notify_all();
        });
        unsigned id = 0;
        auto rpc = [&](const std::string& method, const json& params = json::object()) {
            const auto request_id = ++id;
            protocol.rpc(
                json{{"jsonrpc", "2.0"}, {"id", request_id}, {"method", method}, {"params", params}}
                    .dump());
            const std::scoped_lock lock(mutex);
            require(replies.back().at("id") == request_id, "RPC response ID");
            return replies.back();
        };
        require(rpc("ping").at("result") == "pong", "RPC ping");
        protocol.rpc("{");
        require(replies.back().at("error").at("code") == -32700, "parse error");
        protocol.rpc(R"({"jsonrpc":1,"method":"ping","id":9})");
        require(replies.back().at("error").at("code") == -32600, "invalid request");
        protocol.rpc(R"({"jsonrpc":"2.0","method":"ping","id":[]})");
        require(
            replies.back().at("id").is_null() && replies.back().at("error").at("code") == -32600,
            "invalid ID must not be echoed");
        const auto before_notification = replies.size();
        protocol.rpc(R"({"jsonrpc":"2.0","method":"ping"})");
        require(replies.size() == before_notification, "notification must not get a response");
        require(rpc("notAJetKvmMethod").at("error").at("code") == -32601, "unknown method");
        require(rpc("keyboardReport", {{"keys", {4}}, {"modifier", 256}}).contains("error"),
            "invalid modifier");
        require(
            rpc("keyboardReport", {{"keys", {4}}, {"modifier", 4294967297ull}}).contains("error"),
            "oversized byte cannot wrap");
        require(rpc("wheelReport", {{"wheelY", 1e300}, {"wheelX", 0}}).contains("result"),
            "wheel clamps before integer conversion");
        protocol.hid({1, 1});
        require(messages.at(0) == stream::JetKvmProtocol::Binary({1, 1}), "HID v1 handshake");
        auto idle_events = input->events();
        require(rpc("releaseMouseButtons").at("result") == true, "Release idle mouse buttons");
        require(input->events() == idle_events,
            "Focus loss before movement must not move the TV pointer");
        protocol.hid({5, 4, 1});
        require(rpc("getKeyDownState").at("result").at("keys").at(0) == 4, "HID A pressed");
        bool native_a = false;
        for (auto e : recorded->events()) {
            native_a |= e.type == 1 && e.code == 30 && e.value == 1;
        }
        require(native_a, "HID A must map to evdev KEY_A");
        require(recorded->cursor_hides() > 0, "Keyboard input did not switch pointer mode");
        protocol.hid({2, 2, 5, 0, 0, 0, 0, 0});
        auto state = rpc("getKeyDownState").at("result");
        require(state.at("modifier") == 2 && state.at("keys").at(0) == 5,
            "full report replaces keys and modifiers");
        protocol.hid({2, 0, 0, 0, 0, 0, 0, 0});
        for (auto pair : {std::pair<unsigned, unsigned>{0x2c, 57}, {0x36, 51}, {0x38, 53},
                 {0xe0, 29}, {0xe1, 42}, {0xe2, 56}, {0xe3, 125}, {0xe4, 97}, {0xe5, 54},
                 {0xe6, 100}, {0xe7, 126}}) {
            protocol.hid({5, static_cast<uint8_t>(pair.first), 1});
            const auto events = recorded->events();
            require(events.at(events.size() - 2).code == pair.second, "USB keyboard usage mapping");
            protocol.hid({5, static_cast<uint8_t>(pair.first), 0});
        }
        // Big-endian absolute position: x=16384, y=8192, left button held.
        protocol.hid({3, 0, 0, 64, 0, 0, 0, 32, 0, 1});
        bool x = false;
        bool y = false;
        bool left = false;
        for (auto e : recorded->events()) {
            x |= e.type == 3 && e.code == 0 && e.value == 959;
            y |= e.type == 3 && e.code == 1 && e.value == 269;
            left |= e.type == 1 && e.code == 272 && e.value == 1;
        }
        require(x && y && left, "pointer scaling and left button");
        auto count = input->events();
        protocol.hid({3, 0, 0, 65, 0, 0, 0, 32, 0, 0}, false);
        require(input->events() == count + 3, "unreliable movement cannot release a held button");
        protocol.hid({3, 0, 0, 64, 0, 0, 0, 32, 0, 0});
        count = input->events();
        protocol.hid({3, 0, 0, 65, 0, 0, 0, 32, 0, 1}, false);
        require(input->events() == count + 3,
            "stale unreliable movement cannot re-press a released button");
        count = input->events();
        protocol.hid({3, 0, 0, 65, 0, 0, 0, 32, 0, 0});
        require(input->events() == count,
            "Stationary pointer reports must not refresh the TV idle timer");
        // A button-only press and release must not inject absolute-axis events.
        protocol.hid({3, 0, 0, 65, 0, 0, 0, 32, 0, 1});
        require(input->events() == count + 2, "Stationary press only changes the button");
        count = input->events();
        rpc("releaseMouseButtons");
        require(input->events() == count + 2, "Focus loss only releases the held button");
        const auto released = recorded->events();
        require(released.at(released.size() - 2).type == 1 &&
                    released.at(released.size() - 2).code == 272 &&
                    released.at(released.size() - 2).value == 0,
            "Focus loss releases left mouse button");
        count = input->events();
        rpc("releaseMouseButtons");
        require(input->events() == count, "Repeated focus loss must not generate mouse activity");
        rpc("absMouseReport", {{"x", 100}, {"y", 100}, {"buttons", 2}});
        bool back = false;
        for (auto e : recorded->events()) {
            back |= e.type == 1 && e.code == 412 && e.value == 1;
        }
        require(back, "right mouse button maps to TV Back");
        rpc("absMouseReport", {{"x", 100}, {"y", 100}, {"buttons", 0}});
        for (const auto& malformed : std::vector<stream::JetKvmProtocol::Binary>{
                 {}, {1}, {1, 2}, {2}, {3, 0}, {5, 4}, {7, 0, 255, 255, 255, 255}, {8, 0}}) {
            bool rejected = false;
            try {
                protocol.hid(malformed);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            require(rejected, "malformed HID report accepted");
        }
        protocol.hid({5, 4, 1});
        std::this_thread::sleep_for(550ms);
        protocol.tick();
        require(rpc("getKeyDownState").at("result").at("keys").at(0) == 0,
            "lost keepalive releases keys");
        protocol.hid({5, 4, 1});
        for (int i = 0; i < 12; ++i) {
            std::this_thread::sleep_for(50ms);
            protocol.hid({9});
            protocol.tick();
        }
        require(rpc("getKeyDownState").at("result").at("keys").at(0) == 4,
            "keepalive retains held key");
        protocol.hid({5, 4, 0});
        auto wait_macro = [&](bool running, size_t start) {
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, 2s,
                        [&] {
                            for (size_t i = start; i < messages.size(); ++i) {
                                if (messages[i].size() == 3 && messages[i][0] == 0x34 &&
                                    messages[i][1] == running) {
                                    return true;
                                }
                            }
                            return false;
                        }),
                "macro state acknowledgement");
        };
        size_t start = 0;
        {
            const std::scoped_lock lock(mutex);
            start = messages.size();
        }
        // Two stock wire steps: A for 30 ms, then a full release.
        protocol.hid({7, 0, 0, 0, 0, 2, 0, 4, 0, 0, 0, 0, 0, 0, 30, 0, 0, 0, 0, 0, 0, 0, 0, 0});
        wait_macro(true, start);
        wait_macro(false, start);
        require(rpc("getKeyDownState").at("result").at("keys").at(0) == 0, "macro releases keys");
        {
            const std::scoped_lock lock(mutex);
            start = messages.size();
        }
        protocol.hid({7, 1, 0, 0, 0, 1, 0, 4, 0, 0, 0, 0, 0, 0x13, 0x88});
        wait_macro(true, start);
        protocol.hid({8});
        wait_macro(false, start);
        require(rpc("getKeyDownState").at("result").at("keys").at(0) == 0,
            "cancel releases macro keys");
        for (int i = 0; i < 20; ++i) {
            {
                const std::scoped_lock lock(mutex);
                start = messages.size();
            }
            protocol.hid({7, 0, 0, 0, 0, 1, 0, 4, 0, 0, 0, 0, 0, 0, 50});
            protocol.hid({8});
            wait_macro(true, start);
            wait_macro(false, start);
        }
        require(rpc("setKeyboardLayout", {{"layout", "de-DE"}}).contains("result"), "save layout");
        require(rpc("getKeyboardLayout").at("result") == "de-DE", "read layout");
        const json macros = {{{"id", "test"}, {"name", "Arrow"}, {"steps", json::array()}}};
        require(rpc("setKeyboardMacros", {{"params", {{"macros", macros}}}}).contains("result"),
            "save macros");
        require(rpc("getKeyboardMacros").at("result") == macros, "read macros");
        require(rpc("importBrowserPreferences",
                    {{"keyboardLayout", "fr-FR"}, {"keyboardMacros", json::array()}})
                    .contains("result"),
            "legacy preference import");
        require(rpc("getKeyboardMacros").at("result") == macros &&
                    rpc("getKeyboardLayout").at("result") == "de-DE",
            "import must not overwrite TV console preferences");
        require(saved.read().at("password") == "fixture-password" &&
                    saved.read().at("resolution") == "720p" && saved.read().at("autostart") == true,
            "preferences preserve server settings");
        require(rpc("setStreamQualityFactor", {{"factor", 9}}).at("error").at("code") == -32601,
            "browser cannot change encoder settings");
        protocol.hid({5, 4, 1});
        protocol.stop();
        require(recorded->events().back().type == 0, "stop synchronizes input release");
        count = input->events();
        protocol.hid({5, 5, 1});
        require(input->events() == count, "closed session cannot send input");
        std::cout << "Passed: JSON-RPC, HID v1, evdev mapping, pointer authority, held-key safety, "
                     "macro completion/cancellation, preferences, closed sessions\n";
    }
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
