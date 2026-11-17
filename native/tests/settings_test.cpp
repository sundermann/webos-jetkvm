#include "settings.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using json = nlohmann::json;
static void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
static std::string bytes(const std::filesystem::path& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), {}};
}
int main() try {
    auto pattern =
        (std::filesystem::temp_directory_path() / "webos-jetkvm-settings-XXXXXX").string();
    const char* created = mkdtemp(pattern.data());
    if (!created) {
        return 1;
    }
    const std::filesystem::path folder(created);
    const auto path = folder / "config.json";
    try {
        stream::SettingsStore store(path.string());
        require(
            store.read().at("resolution") == "1080p", "Missing settings did not default to 1080p");
        const json original = {
            {"password", "test-secret"}, {"autostart", true}, {"futureField", 42}};
        {
            std::ofstream file(path);
            file << original;
        }
        require(store.read().at("resolution") == "1080p", "Old settings did not default to 1080p");
        auto result = store.update({{"resolution", "720p"}});
        require(result.at("password") == original.at("password") &&
                    result.at("autostart") == true && result.at("futureField") == 42,
            "Changing resolution lost unrelated settings");
        struct stat mode{};
        require(stat(path.c_str(), &mode) == 0 && (mode.st_mode & 0777u) == 0600,
            "Settings permissions are not private");
        const auto saved = bytes(path);
        for (const auto& invalid : {json{{"resolution", "2160p"}}, json{{"resolution", false}},
                 json{{"autostart", "yes"}}, json{{"password", std::string(1025, 'x')}},
                 json{{"unknown", true}}, json::array()}) {
            bool rejected = false;
            try {
                store.update(invalid);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            require(rejected && bytes(path) == saved,
                "Invalid settings changed the saved configuration");
        }
        const pid_t child = fork();
        require(child >= 0, "Could not fork writer");
        if (child == 0) {
            try {
                stream::SettingsStore(path.string())
                    .update({{"password", ""}, {"autostart", false}});
                _exit(0);
            } catch (...) {
                _exit(1);
            }
        }
        store.update({{"resolution", "540p"}});
        int status = 0;
        require(
            waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "Concurrent writer failed");
        result = store.read();
        require(result.at("resolution") == "540p" && result.at("autostart") == false &&
                    result.at("password") == original.at("password"),
            "Concurrent updates or blank password lost settings");
        for (const auto& preset : stream::resolution_presets()) {
            const auto& resolution = stream::resolution(preset.at("id").get<std::string>());
            require(resolution.width <= 1920 && resolution.height <= 1080 &&
                        resolution.width % 32 == 0 && resolution.height % 4 == 0,
                "Preset exceeds encoder bounds or alignment");
        }
        // Failed writes must retain the existing target and leave no temporary files.
        const auto blocked_path = folder / "blocked.json";
        std::filesystem::create_directory(blocked_path);
        bool failed = false;
        try {
            stream::SettingsStore(blocked_path.string()).update({{"password", "test"}});
        } catch (const std::exception&) {
            failed = true;
        }
        require(failed && std::filesystem::is_directory(blocked_path),
            "Failed settings update replaced the existing target");
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            require(entry.path().filename().string().find("blocked.json.") != 0 ||
                        entry.path().extension() == ".lock",
                "Failed settings update leaked a temporary file");
        }
        const auto bad_lock_path = folder / "unsafe.json";
        std::filesystem::create_symlink(path, bad_lock_path.string() + ".lock");
        failed = false;
        const auto before_failure = bytes(path);
        try {
            stream::SettingsStore(bad_lock_path.string()).update({{"password", "test"}});
        } catch (const std::runtime_error&) {
            failed = true;
        }
        require(failed && bytes(path) == before_failure,
            "Settings followed a symlink lock or damaged another file");
        failed = false;
        try {
            (void)stream::SettingsStore((path / "nested.json").string()).read();
        } catch (const std::runtime_error&) {
            failed = true;
        }
        require(failed, "A non-directory settings parent silently returned defaults");
        {
            std::ofstream file(path);
            file << "{\"password\":\"private-invalid-json";
        }
        try {
            (void)store.read();
            throw std::runtime_error("Malformed settings were accepted");
        } catch (const std::invalid_argument& error) {
            require(std::string(error.what()).find("private") == std::string::npos,
                "Parse error exposed private settings");
        }
        std::filesystem::remove_all(folder);
        std::cout << "Passed: migration defaults, bounds, validation, private permissions, "
                     "concurrent updates, password preservation\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(folder);
        std::cerr << error.what() << '\n';
        return 1;
    }
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
