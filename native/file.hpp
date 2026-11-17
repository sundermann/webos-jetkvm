#pragma once

#include <unistd.h>
#include <utility>

namespace stream {
// Close each descriptor exactly once, including exception and early-return paths.
class UniqueFd {
    int value_ = -1;

  public:
    explicit UniqueFd(int value = -1) noexcept : value_(value) {}
    ~UniqueFd() {
        close();
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            close();
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    [[nodiscard]] int get() const noexcept {
        return value_;
    }
    int close() noexcept {
        const int descriptor = std::exchange(value_, -1);
        return descriptor < 0 ? 0 : ::close(descriptor);
    }
};
} // namespace stream
