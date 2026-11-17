#include "cursor.hpp"

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <wayland-client.h>
#include <wayland-webos-input-manager-client-protocol.h>

namespace stream {
struct Cursor::State {
    std::unique_ptr<wl_display, decltype(&wl_display_disconnect)> display{
        nullptr, wl_display_disconnect};
    std::unique_ptr<wl_webos_input_manager, decltype(&wl_webos_input_manager_destroy)> manager{
        nullptr, wl_webos_input_manager_destroy};
    uint32_t visibility_opcode = 0;
    static void global(
        void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
        if (std::strcmp(interface, wl_webos_input_manager_interface.name) == 0) {
            auto& state = *static_cast<State*>(data);
            state.manager.reset(static_cast<wl_webos_input_manager*>(
                wl_registry_bind(registry, name, &wl_webos_input_manager_interface, 1)));
        }
    }
    static void removed(void*, wl_registry*, uint32_t) {}
    static void visibility(void*, wl_webos_input_manager*, uint32_t, wl_webos_seat*) {}
    State() {
        // Native services do not always inherit the compositor's XDG environment.
        const char* socket = std::getenv("WAYLAND_DISPLAY") || std::getenv("XDG_RUNTIME_DIR")
                                 ? nullptr
                                 : "/tmp/xdg/wayland-0";
        display.reset(wl_display_connect(socket));
        if (!display) {
            throw std::runtime_error("Cannot connect to TV pointer control");
        }
        std::unique_ptr<wl_registry, decltype(&wl_registry_destroy)> registry(
            wl_display_get_registry(display.get()), wl_registry_destroy);
        static const wl_registry_listener listener{global, removed};
        wl_registry_add_listener(registry.get(), &listener, this);
        if (wl_display_roundtrip(display.get()) < 0 || !manager) {
            throw std::runtime_error("TV pointer control is unavailable");
        }
        static const wl_webos_input_manager_listener cursor_listener{visibility};
        wl_webos_input_manager_add_listener(manager.get(), &cursor_listener, this);
        // Retail webOS and the OSE SDK order these requests differently. Resolve
        // the opcode from the TV's protocol library instead of its SDK header.
        for (int opcode = 0; opcode < wl_webos_input_manager_interface.method_count; ++opcode) {
            const auto& request = wl_webos_input_manager_interface.methods[opcode];
            if (std::strcmp(request.name, "set_cursor_visibility") == 0 &&
                std::strcmp(request.signature, "u") == 0) {
                visibility_opcode = static_cast<uint32_t>(opcode);
                return;
            }
        }
        throw std::runtime_error("TV pointer visibility request is unavailable");
    }
    void hide() {
        wl_proxy_marshal(
            reinterpret_cast<wl_proxy*>(manager.get()), visibility_opcode, uint32_t{0});
        // Finish the mode switch before injecting the key or click that follows.
        if (wl_display_roundtrip(display.get()) < 0) {
            throw std::runtime_error("TV pointer mode change failed");
        }
    }
};
Cursor::Cursor() : state_(std::make_unique<State>()) {}
Cursor::~Cursor() = default;
Cursor::Cursor(Cursor&&) noexcept = default;
Cursor& Cursor::operator=(Cursor&&) noexcept = default;
void Cursor::hide() {
    state_->hide();
}
} // namespace stream
