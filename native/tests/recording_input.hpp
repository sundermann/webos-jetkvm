#pragma once

#include "input_device.hpp"

#include <deque>
#include <mutex>
#include <vector>

// Test-only observer for the input logic. It does not implement a server,
// compositor, capture source or platform backend and is never linked to the TV.
class RecordingInput final : public stream::InputDevice {
  public:
    struct Event {
        unsigned type = 0;
        unsigned code = 0;
        int value = 0;
    };

  private:
    // Guards recorded events and cursor transitions across the macro worker.
    mutable std::mutex mutex_;
    std::deque<Event> events_;
    unsigned hidden_ = 0;

  public:
    void write_event(unsigned type, unsigned code, int value) override {
        const std::scoped_lock lock(mutex_);
        if (events_.size() == 256) {
            events_.pop_front();
        }
        events_.push_back({type, code, value});
    }
    void hide_cursor() override {
        const std::scoped_lock lock(mutex_);
        ++hidden_;
    }
    [[nodiscard]] std::vector<Event> events() const {
        const std::scoped_lock lock(mutex_);
        return {events_.begin(), events_.end()};
    }
    [[nodiscard]] unsigned cursor_hides() const {
        const std::scoped_lock lock(mutex_);
        return hidden_;
    }
};
