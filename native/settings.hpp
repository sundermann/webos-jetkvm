#pragma once
#include "video.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace stream {
class SettingsStore {
    std::string path_;
    nlohmann::json save(const nlohmann::json& patch, bool require_password) const;

  public:
    explicit SettingsStore(std::string path) : path_(std::move(path)) {}
    [[nodiscard]] nlohmann::json read() const;
    nlohmann::json update(const nlohmann::json& patch) const;
    nlohmann::json update_console(const nlohmann::json& patch) const;
};
nlohmann::json resolution_presets();
} // namespace stream
