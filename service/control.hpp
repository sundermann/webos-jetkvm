#pragma once

#include "settings.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace stream {
// Luna-independent operations. The adapter owns the event loop and supplies
// authorization; all settings, process and boot paths are explicit dependencies.
class ControlService {
  public:
    struct Options {
        std::string directory;
        std::string boot_link;
        std::string pid_path;
    };

  private:
    const Options options_;
    const std::string executable_;
    SettingsStore settings_;
    std::function<void(std::string)> launch_;
    [[nodiscard]] std::optional<pid_t> daemon_pid() const;
    [[nodiscard]] nlohmann::json stream_status() const;
    void start() const;
    void stop() const;
    void autostart(bool enabled) const;

  public:
    ControlService(Options options, std::function<void(std::string)> launch);
    [[nodiscard]] nlohmann::json dispatch(
        std::string_view method, const nlohmann::json& request, bool root) const;
};
} // namespace stream
