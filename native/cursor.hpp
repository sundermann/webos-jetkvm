#pragma once
#include <memory>

namespace stream {
// Changes the compositor's pointer mode, including its notification to TV apps.
class Cursor {
    struct State;
    std::unique_ptr<State> state_;

  public:
    Cursor();
    ~Cursor();
    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;
    Cursor(Cursor&&) noexcept;
    Cursor& operator=(Cursor&&) noexcept;
    void hide();
};
} // namespace stream
