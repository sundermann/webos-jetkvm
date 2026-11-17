#include "file.hpp"
#include "server.hpp"
#include "settings.hpp"
#include "signals.hpp"

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <charconv>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <httplib.h>
#include <ifaddrs.h>
#include <iostream>
#include <iterator>
#include <limits.h>
#include <memory>
#include <openssl/crypto.h>
#include <rtc/rtc.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
using json = nlohmann::json;
using namespace std::chrono_literals;
static std::string password_header(const std::string& path) {
    const auto config = stream::SettingsStore(path).read();
    const auto password = config.value("password", std::string{});
    if (password.empty()) {
        throw std::runtime_error("Set a password before starting browser access");
    }
    return httplib::make_basic_authentication_header("jetkvm", password).second;
}
static std::set<std::string> local_addresses() {
    std::set<std::string> hosts{"localhost", "127.0.0.1"};
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        const std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> owner(interfaces, freeifaddrs);
        for (auto* it = interfaces; it; it = it->ifa_next) {
            if (it->ifa_addr && it->ifa_addr->sa_family == AF_INET) {
                std::array<char, INET_ADDRSTRLEN> address{};
                sockaddr_in ipv4{};
                std::memcpy(&ipv4, it->ifa_addr, sizeof(ipv4));
                if (inet_ntop(AF_INET, &ipv4.sin_addr, address.data(), address.size())) {
                    hosts.insert(address.data());
                }
            }
        }
    }
    return hosts;
}

static unsigned number(const std::string& value) {
    unsigned result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        throw std::invalid_argument("Expected an unsigned decimal number");
    }
    return result;
}

int main(int argc, char** argv) try {
    unsigned port = 47990;
    unsigned bitrate = 8000;
    bool daemon = false;
    std::string config_path =
        "/media/developer/apps/usr/palm/services/org.webosbrew.jetkvm.service/config.json";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--daemon") {
            daemon = true;
        } else if (arg == "--port" && i + 1 < argc) {
            port = number(argv[++i]);
        } else if (arg == "--bitrate" && i + 1 < argc) {
            bitrate = number(argv[++i]);
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else {
            std::cout
                << "webos-jetkvm [--daemon] [--port 47990] [--bitrate 8000] [--config FILE]\n";
            return arg == "--help" ? 0 : 2;
        }
    }
    if (port < 1024 || port > 65535 || bitrate < 1000 || bitrate > 30000) {
        throw std::runtime_error("Port or bitrate out of range");
    }
    (void)password_header(config_path);
    stream::UniqueFd lock;
    if (daemon) {
        const std::string path = "/tmp/webos-jetkvm-" + std::to_string(port) + ".pid";
        lock = stream::UniqueFd(open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600));
        if (lock.get() < 0) {
            throw std::runtime_error("Cannot open daemon lock");
        }
        if (flock(lock.get(), LOCK_EX | LOCK_NB) < 0) {
            return 0; // already running
        }
        pid_t pid = fork();
        if (pid < 0) {
            throw std::runtime_error("Cannot fork");
        }
        if (pid > 0) {
            return 0;
        }
        setsid();
        const std::string log = "/tmp/webos-jetkvm.log";
        const stream::UniqueFd out(
            open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600));
        const stream::UniqueFd in(open("/dev/null", O_RDONLY));
        if (out.get() < 0 || in.get() < 0) {
            return 1;
        }
        dup2(in.get(), 0);
        dup2(out.get(), 1);
        dup2(out.get(), 2);
        ftruncate(lock.get(), 0);
        const auto text = std::to_string(getpid()) + "\n";
        (void)write(lock.get(), text.data(), text.size());
    }
    const stream::TerminationSignals termination;
    std::signal(SIGPIPE, SIG_IGN);
    rtc::InitLogger(rtc::LogLevel::Warning);
    stream::Server app(bitrate, config_path);

    const auto hosts = local_addresses();
    std::array<char, PATH_MAX> executable{};
    const auto executable_length =
        readlink("/proc/self/exe", executable.data(), executable.size() - 1);
    if (executable_length < 0) {
        throw std::runtime_error("Cannot locate web assets");
    }
    const std::string executable_path(executable.data());
    const auto web_dir = executable_path.substr(0, executable_path.find_last_of('/')) + "/web";
    std::ifstream web_index(web_dir + "/index.html");
    const std::string jetkvm_html{std::istreambuf_iterator<char>(web_index), {}};
    if (jetkvm_html.empty()) {
        throw std::runtime_error("JetKVM web assets are missing beside the executable");
    }
    httplib::Server http;
    // The old and new app IDs can coexist. Do not share the HTTP port:
    // SO_REUSEPORT would distribute authenticated requests across both servers.
    http.set_socket_options([](auto fd) {
        const int reuse = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    });
    http.set_payload_max_length(128UL * 1024UL);
    http.set_read_timeout(15, 0);
    http.set_write_timeout(15, 0);
    http.set_pre_routing_handler([&](const httplib::Request& req, httplib::Response& res) {
        const auto host = req.get_header_value("Host");
        if (hosts.count(host.substr(0, host.find(':'))) == 0) {
            res.status = 403;
            return httplib::Server::HandlerResponse::Handled;
        }
        bool accepted = false;
        try {
            const auto expected = password_header(config_path);
            const auto supplied = req.get_header_value("Authorization");
            accepted = supplied.size() == expected.size() &&
                       CRYPTO_memcmp(supplied.data(), expected.data(), expected.size()) == 0;
        } catch (const std::exception&) {
            accepted = false; // Unreadable settings must fail authentication.
        }
        if (!accepted) {
            res.status = 401;
            res.set_header("WWW-Authenticate", "Basic realm=\"webOS JetKVM\"");
            res.set_content(
                "Log in as jetkvm with the password configured in the TV app.", "text/plain");
            return httplib::Server::HandlerResponse::Handled;
        }
        if (req.path == "/webrtc/signaling/client" &&
            req.get_header_value("Origin") != "http://" + host) {
            res.status = 403;
            return httplib::Server::HandlerResponse::Handled;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_header("Content-Security-Policy",
            "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
            "img-src 'self' data: blob:; font-src 'self'; connect-src 'self'; media-src "
            "'self' blob:; frame-ancestors 'none'");
        return httplib::Server::HandlerResponse::Unhandled;
    });
    auto index = [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(jetkvm_html, "text/html; charset=utf-8");
    };
    http.Get("/", index);
    http.Get("/settings(/.*)?", index);
    // Keep the independent native implementation's assets outside the GPL UI.
    if (!http.set_mount_point("/assets", web_dir + "/assets")) {
        throw std::runtime_error("Cannot mount JetKVM assets");
    }
    // Preserve the upstream public icons and manifest alongside the UI assets.
    for (const char* file : {"favicon.ico", "favicon.svg", "favicon.png", "favicon-96x96.png",
             "apple-touch-icon.png", "web-app-manifest-192x192.png", "web-app-manifest-512x512.png",
             "jetkvm.svg", "site.webmanifest"}) {
        const auto serve = [&web_dir, file](const httplib::Request&, httplib::Response& res) {
            res.set_file_content(web_dir + "/" + file,
                std::string_view(file) == "site.webmanifest" ? "application/manifest+json" : "");
        };
        http.Get(std::string("/") + file, serve);
        if (std::string_view(file) == "site.webmanifest") {
            http.Get(std::string("/public/") + file, serve);
        }
    }
    http.Get("/api/status",
        [&](const auto&, auto& res) { res.set_content(app.status().dump(), "application/json"); });
    http.WebSocket("/webrtc/signaling/client", [&](const auto&, auto& ws) { app.signaling(ws); });
    constexpr const char* kBindAddress = "0.0.0.0";
    if (!http.bind_to_port(kBindAddress, static_cast<int>(port))) {
        throw std::runtime_error("Cannot bind HTTP port");
    }
    std::atomic<bool> stopping{false};
    bool listened = false;
    std::jthread listener([&](const std::stop_token& stop) {
        const std::stop_callback shutdown(stop, [&] {
            stopping = true;
            http.stop();
        });
        listened = http.listen_after_bind();
        if (!stopping) {
            if (kill(getpid(), SIGTERM) < 0) {
                std::cerr << "Cannot signal HTTP listener failure\n";
            }
        }
    });
    std::cout << "webos-jetkvm listening on " << kBindAddress << ':' << port << '\n' << std::flush;
    termination.wait();
    stopping = true;
    app.stop();
    http.stop();
    listener.join();
    if (!listened) {
        throw std::runtime_error("HTTP listener failed");
    }
    return 0;
} catch (const std::exception& e) {
    std::cerr << "webos-jetkvm: " << e.what() << '\n';
    return 1;
}
