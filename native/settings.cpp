#include "settings.hpp"

#include "file.hpp"

#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <sys/file.h>
#include <unistd.h>

using json = nlohmann::json;
namespace stream {
namespace {
class TemporarySettings {
    std::string path_;

  public:
    UniqueFd descriptor;
    explicit TemporarySettings(const std::string& destination) : path_(destination + ".XXXXXX") {
        descriptor = UniqueFd(mkstemp(path_.data()));
        if (descriptor.get() < 0) {
            throw std::runtime_error("Cannot save settings");
        }
    }
    TemporarySettings(const TemporarySettings&) = delete;
    TemporarySettings& operator=(const TemporarySettings&) = delete;
    TemporarySettings(TemporarySettings&&) = delete;
    TemporarySettings& operator=(TemporarySettings&&) = delete;
    ~TemporarySettings() {
        if (!path_.empty()) {
            unlink(path_.c_str());
        }
    }
    void commit(const std::string& destination) {
        if (fsync(descriptor.get()) < 0) {
            throw std::runtime_error("Cannot sync settings");
        }
        if (descriptor.close() < 0 || rename(path_.c_str(), destination.c_str()) < 0) {
            throw std::runtime_error("Cannot commit settings");
        }
        path_.clear();
    }
};
void validate(const json& value) {
    if (!value.is_object()) {
        throw std::invalid_argument("Settings must be an object");
    }
    if (!value.at("password").is_string() ||
        value.at("password").get_ref<const std::string&>().size() > 1024) {
        throw std::invalid_argument("Password must be text of at most 1024 bytes");
    }
    if (!value.at("autostart").is_boolean()) {
        throw std::invalid_argument("Autostart must be true or false");
    }
    if (!value.at("resolution").is_string()) {
        throw std::invalid_argument("Resolution must be a preset name");
    }
    (void)resolution(value.at("resolution").get_ref<const std::string&>());
}
} // namespace

json resolution_presets() {
    auto presets = json::array();
    for (const auto& preset : kResolutions) {
        presets.push_back({{"id", preset.id}, {"width", preset.width}, {"height", preset.height},
            {"fps", preset.fps}});
    }
    return presets;
}

json SettingsStore::read() const {
    json value = {{"password", ""}, {"autostart", false}, {"resolution", "1080p"}};
    std::ifstream file(path_);
    if (file) {
        json saved;
        try {
            saved = json::parse(file);
        } catch (const json::exception&) {
            throw std::invalid_argument("Saved settings are invalid JSON");
        }
        if (!saved.is_object()) {
            throw std::invalid_argument("Saved settings must be an object");
        }
        value.update(saved);
    } else if (access(path_.c_str(), F_OK) == 0 || errno != ENOENT) {
        throw std::runtime_error("Cannot read settings");
    }
    validate(value);
    return value;
}

json SettingsStore::update(const json& patch) const {
    if (!patch.is_object()) {
        throw std::invalid_argument("Settings must be an object");
    }
    for (const auto& item : patch.items()) {
        if (item.key() != "password" && item.key() != "autostart" && item.key() != "resolution") {
            throw std::invalid_argument("Unknown setting: " + item.key());
        }
    }
    return save(patch, true);
}

json SettingsStore::update_console(const json& patch) const {
    if (!patch.is_object()) {
        throw std::invalid_argument("Console preferences must be an object");
    }
    for (const auto& item : patch.items()) {
        if (item.key() == "keyboardLayout") {
            if (!item.value().is_string() ||
                item.value().get_ref<const std::string&>().size() > 32) {
                throw std::invalid_argument("Invalid keyboard layout");
            }
        } else if (item.key() == "keyboardMacros") {
            if (!item.value().is_array() || item.value().size() > 500 ||
                item.value().dump().size() > 60UL * 1024UL) {
                throw std::invalid_argument("Invalid keyboard macros");
            }
            for (const auto& macro : item.value()) {
                if (!macro.is_object() || !macro.contains("id") || !macro.at("id").is_string() ||
                    !macro.contains("name") || !macro.at("name").is_string() ||
                    (macro.contains("steps") && !macro.at("steps").is_array())) {
                    throw std::invalid_argument("Invalid keyboard macro");
                }
            }
        } else {
            throw std::invalid_argument("Unknown console preference");
        }
    }
    return save({{"console", patch}}, false).at("console");
}

json SettingsStore::save(const json& patch, bool require_password) const {
    const UniqueFd lock(
        open((path_ + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (lock.get() < 0) {
        throw std::runtime_error("Cannot lock settings");
    }
    if (flock(lock.get(), LOCK_EX) < 0) {
        throw std::runtime_error("Cannot lock settings");
    }
    auto next = read();
    auto changes = patch;
    if (changes.contains("password") && changes.at("password") == "") {
        changes.erase("password");
    }
    if (changes.contains("console")) {
        auto preferences = next.value("console", json::object());
        preferences.update(changes.at("console"));
        changes["console"] = std::move(preferences);
    }
    next.update(changes);
    validate(next);
    if (require_password && next.at("password").get_ref<const std::string&>().empty()) {
        throw std::invalid_argument("Enter a password before saving settings");
    }
    TemporarySettings temporary(path_);
    const auto bytes = next.dump(2) + "\n";
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count =
            write(temporary.descriptor.get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::runtime_error("Cannot write settings");
        }
        offset += static_cast<size_t>(count);
    }
    temporary.commit(path_);
    return next;
}
} // namespace stream
