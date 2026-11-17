#pragma once

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>

class TemporaryDirectory {
    std::filesystem::path path_;

  public:
    TemporaryDirectory() {
        auto pattern =
            (std::filesystem::temp_directory_path() / "webos-jetkvm-test-XXXXXX").string();
        const auto* created = mkdtemp(pattern.data());
        if (!created) {
            throw std::runtime_error("Cannot create test directory");
        }
        path_ = created;
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }
};
