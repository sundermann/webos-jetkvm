#include "control.hpp"

#include <arpa/inet.h>
#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <httplib.h>
#include <ifaddrs.h>
#include <memory>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

using json = nlohmann::json;
namespace stream {
ControlService::ControlService(Options options, std::function<void(std::string)> launch)
    : options_(std::move(options)), executable_(options_.directory + "/webos-jetkvm"),
      settings_(options_.directory + "/config.json"), launch_(std::move(launch)) {
    assert(launch_);
}
std::optional<pid_t> ControlService::daemon_pid() const {
    long value = 0;
    std::ifstream file(options_.pid_path);
    file >> value;
    if (value <= 1 || value > INT_MAX) {
        return std::nullopt;
    }
    std::array<char, PATH_MAX> path{};
    const auto length =
        readlink(("/proc/" + std::to_string(value) + "/exe").c_str(), path.data(), path.size() - 1);
    if (length <= 0) {
        return std::nullopt;
    }
    const std::string actual(path.data(), static_cast<size_t>(length));
    if (actual == executable_ || actual == executable_ + " (deleted)") {
        return static_cast<pid_t>(value);
    }
    return std::nullopt;
}
json ControlService::stream_status() const {
    const auto current = settings_.read();
    if (!daemon_pid() || current.value("password", "").empty()) {
        return json::object();
    }
    httplib::Client client("127.0.0.1", 47990);
    client.set_connection_timeout(0, 200000);
    client.set_read_timeout(0, 300000);
    client.set_basic_auth("jetkvm", current.at("password").get<std::string>());
    auto response = client.Get("/api/status");
    if (!response || response->status != 200) {
        return json::object();
    }
    auto status = json::parse(response->body, nullptr, false);
    return status.is_object() && status.value("application", "") == "org.webosbrew.jetkvm"
               ? status
               : json::object();
}
void ControlService::start() const {
    const auto current = settings_.read();
    if (current.value("password", "").empty()) {
        throw std::runtime_error("Save a password before starting browser access");
    }
    if (!daemon_pid()) {
        launch_(executable_);
    }
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (!stream_status().empty()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw std::runtime_error("Browser server did not become ready. Check /tmp/webos-jetkvm.log and "
                             "whether port 47990 is in use.");
}
void ControlService::stop() const {
    const auto pid = daemon_pid();
    if (!pid) {
        return;
    }
    if (kill(*pid, SIGTERM) < 0 && errno != ESRCH) {
        throw std::runtime_error("Could not stop browser server");
    }
    for (int attempt = 0; attempt < 60; ++attempt) {
        if (daemon_pid() != pid) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw std::runtime_error("Browser server is still shutting down; retry shortly");
}
void ControlService::autostart(bool enabled) const {
    if (enabled) {
        if (mkdir(std::filesystem::path(options_.boot_link).parent_path().c_str(), 0755) < 0 &&
            errno != EEXIST) {
            throw std::runtime_error("Cannot create autostart directory");
        }
        if (unlink(options_.boot_link.c_str()) < 0 && errno != ENOENT) {
            throw std::runtime_error("Cannot update autostart entry");
        }
        if (symlink((options_.directory + "/autostart.sh").c_str(), options_.boot_link.c_str()) <
            0) {
            throw std::runtime_error("Cannot enable autostart");
        }
    } else if (unlink(options_.boot_link.c_str()) < 0 && errno != ENOENT) {
        throw std::runtime_error("Cannot disable autostart");
    }
}
static json urls() {
    std::set<std::string> addresses;
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        const std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> owner(interfaces, freeifaddrs);
        for (auto* it = interfaces; it; it = it->ifa_next) {
            if (it->ifa_addr && it->ifa_addr->sa_family == AF_INET) {
                std::array<char, INET_ADDRSTRLEN> address{};
                sockaddr_in ipv4{};
                std::memcpy(&ipv4, it->ifa_addr, sizeof(ipv4));
                if (inet_ntop(AF_INET, &ipv4.sin_addr, address.data(), address.size()) &&
                    std::strncmp(address.data(), "127.", 4) != 0) {
                    addresses.insert("http://" + std::string(address.data()) + ":47990/");
                }
            }
        }
    }
    return json(addresses);
}
json ControlService::dispatch(std::string_view method, const json& request, bool root) const {
    json reply;
    try {
        if (method != "status" && method != "quit" && !root) {
            throw std::runtime_error(
                "Root access is required; open the TV app to elevate this service");
        }
        if (method == "start") {
            start();
            reply = {{"returnValue", true}, {"message", "Browser server started"}};
        } else if (method == "stop") {
            stop();
            reply = {{"returnValue", true}, {"message", "Browser server stopped"}};
        } else if (method == "configure") {
            const auto next = settings_.update(request);
            autostart(next.at("autostart").get<bool>());
            reply = {{"returnValue", true}, {"message", "Settings saved"}};
        } else if (method == "status") {
            const auto current = settings_.read();
            const auto status = stream_status();
            reply = {{"returnValue", true}, {"root", root}, {"running", daemon_pid().has_value()},
                {"ready", !status.empty()}, {"streaming", status.value("streaming", false)},
                {"error", status.value("error", "")}, {"urls", urls()},
                {"videoPresets", stream::resolution_presets()},
                {"settings", {{"passwordConfigured", !current.value("password", "").empty()},
                                 {"autostart", current.value("autostart", false)},
                                 {"resolution", current.at("resolution")}}}};
        } else if (method == "quit") {
            reply = {{"returnValue", true}};
        } else {
            throw std::runtime_error("Unknown method");
        }
    } catch (const std::exception& error) {
        reply = {{"returnValue", false}, {"errorText", error.what()}};
    }
    return reply;
}
} // namespace stream
