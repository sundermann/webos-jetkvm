#include "control.hpp"
#include "resource.hpp"

#include <array>
#include <glib.h>
#include <iostream>
#include <luna-service2/lunaservice.h>
#include <memory>
#include <stdexcept>
#include <unistd.h>

namespace {
constexpr const char* kServiceName = "org.webosbrew.jetkvm.service";
constexpr const char* kDirectory =
    "/media/developer/apps/usr/palm/services/org.webosbrew.jetkvm.service";

class LunaError {
    LSError value_{};

  public:
    LunaError() {
        LSErrorInit(&value_);
    }
    ~LunaError() {
        LSErrorFree(&value_);
    }
    LunaError(const LunaError&) = delete;
    LunaError& operator=(const LunaError&) = delete;
    LunaError(LunaError&&) = delete;
    LunaError& operator=(LunaError&&) = delete;
    LSError* get() noexcept {
        return &value_;
    }
};
struct UnregisterService {
    void operator()(LSHandle* handle) const noexcept {
        LunaError error;
        if (!LSUnregister(handle, error.get())) {
            std::cerr << "Could not unregister Luna service\n";
        }
    }
};
// The source cannot outlive its loop or the callback's context, including when
// registration fails or a second quit request arrives before the first timeout.
class QuitSource {
    GMainLoop& loop_;
    guint id_ = 0;

  public:
    explicit QuitSource(GMainLoop& loop) : loop_(loop) {}
    ~QuitSource() {
        if (id_ != 0) {
            g_source_remove(id_);
        }
    }
    QuitSource(const QuitSource&) = delete;
    QuitSource& operator=(const QuitSource&) = delete;
    QuitSource(QuitSource&&) = delete;
    QuitSource& operator=(QuitSource&&) = delete;
    void schedule() {
        if (id_ == 0) {
            id_ = g_timeout_add(
                100,
                [](gpointer context) -> gboolean {
                    auto& source = *static_cast<QuitSource*>(context);
                    source.id_ = 0;
                    g_main_loop_quit(&source.loop_);
                    return G_SOURCE_REMOVE;
                },
                this);
        }
    }
};
void launch_browser(std::string executable) {
    std::string daemon = "--daemon";
    std::array<gchar*, 3> arguments{executable.data(), daemon.data(), nullptr};
    GError* raw_error = nullptr;
    gint status = 0;
    // GLib closes unrelated descriptors before exec, preserving Luna's existing
    // process-launch behavior. Own its error before any C++ operation may throw.
    const bool spawned = g_spawn_sync(nullptr, arguments.data(), nullptr,
        static_cast<GSpawnFlags>(G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL), nullptr,
        nullptr, nullptr, nullptr, &status, &raw_error);
    const std::unique_ptr<GError, decltype(&g_error_free)> error(raw_error, g_error_free);
    if (!spawned || status != 0) {
        throw std::runtime_error("Could not start webOS JetKVM; check /tmp/webos-jetkvm.log");
    }
}
struct ServiceContext {
    stream::ControlService control{
        {kDirectory, "/var/lib/webosbrew/init.d/webos-jetkvm", "/tmp/webos-jetkvm-47990.pid"},
        launch_browser};
    QuitSource quit;
    explicit ServiceContext(GMainLoop& loop) : quit(loop) {}
};
bool dispatch(LSHandle* handle, LSMessage* message, void* data) noexcept {
    try {
        auto& context = *static_cast<ServiceContext*>(data);
        const std::string_view method = LSMessageGetMethod(message);
        nlohmann::json reply;
        try {
            const auto request = method == "configure"
                                     ? nlohmann::json::parse(LSMessageGetPayload(message))
                                     : nlohmann::json::object();
            reply = context.control.dispatch(method, request, geteuid() == 0);
        } catch (const std::exception&) {
            reply = {{"returnValue", false}, {"errorText", "Invalid request payload"}};
        }
        LunaError error;
        const bool sent = LSMessageReply(handle, message, reply.dump().c_str(), error.get());
        if (!sent) {
            std::cerr << "Could not reply to Luna request\n";
        }
        if (method == "quit") {
            context.quit.schedule();
        }
        return sent;
    } catch (const std::exception& error) {
        std::cerr << "Luna request failed: " << error.what() << '\n';
        return false;
    }
}
} // namespace
int main() try {
    const std::unique_ptr<GMainLoop, decltype(&g_main_loop_unref)> loop(
        g_main_loop_new(nullptr, FALSE), g_main_loop_unref);
    if (!loop) {
        throw std::runtime_error("Cannot create Luna service event loop");
    }
    ServiceContext context(*loop);
    LunaError error;
    const auto handle = stream::acquire_resource<LSHandle>(
        [&](LSHandle** output) noexcept { return LSRegister(kServiceName, output, error.get()); },
        UnregisterService{}, "Cannot register webOS JetKVM Luna service");
    std::array<LSMethod, 6> methods{{{"status", dispatch, LUNA_METHOD_FLAGS_NONE},
        {"configure", dispatch, LUNA_METHOD_FLAGS_NONE},
        {"start", dispatch, LUNA_METHOD_FLAGS_NONE}, {"stop", dispatch, LUNA_METHOD_FLAGS_NONE},
        {"quit", dispatch, LUNA_METHOD_FLAGS_NONE}, {nullptr, nullptr, LUNA_METHOD_FLAGS_NONE}}};
    if (!LSRegisterCategory(handle.get(), "/", methods.data(), nullptr, nullptr, error.get()) ||
        !LSCategorySetData(handle.get(), "/", &context, error.get()) ||
        !LSGmainAttach(handle.get(), loop.get(), error.get())) {
        throw std::runtime_error("Cannot attach webOS JetKVM Luna service");
    }
    g_main_loop_run(loop.get());
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
