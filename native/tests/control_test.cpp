#include "control.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
} // namespace
int main() try {
    const TemporaryDirectory first_directory;
    const TemporaryDirectory second_directory;
    const auto make_options = [](const TemporaryDirectory& directory) {
        return stream::ControlService::Options{directory.path().string(),
            (directory.path() / "init.d/jetkvm").string(),
            (directory.path() / "server.pid").string()};
    };
    unsigned launches = 0;
    const auto launch = [&](const std::string& executable) {
        ++launches;
        require(executable == (first_directory.path() / "webos-jetkvm").string(),
            "Launch dependency received the wrong executable");
        throw std::runtime_error("Injected process launch failure");
    };
    stream::ControlService first(make_options(first_directory), launch);
    stream::ControlService second(make_options(second_directory), launch);
    require(!first.dispatch("start", {}, true).at("returnValue"), "Started without a password");
    require(!first.dispatch("configure", {{"password", "test"}}, false).at("returnValue"),
        "Unprivileged settings update was accepted");
    require(launches == 0, "A rejected request invoked the process launcher");
    require(first.dispatch("configure", {{"password", "test"}, {"autostart", true}}, true)
                .at("returnValue"),
        "Could not enable autostart");
    require(std::filesystem::read_symlink(first_directory.path() / "init.d/jetkvm") ==
                first_directory.path() / "autostart.sh",
        "Incorrect boot target");
    require(first.dispatch("status", {}, false).at("settings").at("passwordConfigured"),
        "Saved settings were not visible");
    require(!second.dispatch("status", {}, false).at("settings").at("passwordConfigured"),
        "Service instances shared mutable settings");
    require(!first.dispatch("start", {}, true).at("returnValue"),
        "Missing daemon executable was accepted");
    require(launches == 1, "Process launch failure was not propagated");
    require(first.dispatch("configure", {{"autostart", false}, {"resolution", "720p"}}, true)
                .at("returnValue"),
        "Could not disable autostart");
    require(!std::filesystem::exists(first_directory.path() / "init.d/jetkvm"),
        "Disabled boot link still exists");
    const auto status = first.dispatch("status", {}, false);
    require(status.at("settings").at("passwordConfigured") &&
                status.at("settings").at("resolution") == "720p",
        "Configuration lost fields");
    require(status.dump().find("\"test\"") == std::string::npos, "Status exposed the password");
    require(first.dispatch("stop", {}, true).at("returnValue"), "Stopping absent daemon failed");
    require(first.dispatch("quit", {}, false).at("returnValue"), "Unprivileged quit rejected");
    require(!first.dispatch("unknown", {}, true).at("returnValue"), "Unknown method accepted");
    std::cout << "Passed: independent service contexts, authorization, boot links and failures\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
