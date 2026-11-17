#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace stream {
// Exception approved for the Nyx ABI: its callback has no user-data argument.
// Only the registration slot is static. Each registration owns its callback and
// drains in-flight calls before its dependencies may be destroyed. Detach before
// calling the driver's stop/close: those functions may wait for a callback.
template <class Signature, auto Rejected> class CallbackBridge;
template <auto Rejected, class Result, class... Args>
class CallbackBridge<Result(Args...), Rejected> {
    struct State {
        // Guards callback execution and enabled; destruction waits for this lock.
        std::mutex mutex;
        bool enabled = true;
        std::function<Result(Args...)> callback;
        explicit State(std::function<Result(Args...)> value) : callback(std::move(value)) {}
    };
    struct Slot {
        // Guards active. Release before taking State::mutex or calling the SDK.
        std::mutex mutex;
        std::shared_ptr<State> active;
    };
    // This mutex-backed slot works with both libc++ and libstdc++; their
    // supported shared_ptr atomic APIs differ across the target SDKs.
    inline static Slot slot_;
    std::shared_ptr<State> state_;

  public:
    explicit CallbackBridge(std::function<Result(Args...)> callback)
        : state_(std::make_shared<State>(std::move(callback))) {}
    ~CallbackBridge() {
        {
            const std::scoped_lock lock(slot_.mutex);
            if (slot_.active == state_) {
                slot_.active.reset();
            }
        }
        const std::scoped_lock lock(state_->mutex);
        state_->enabled = false;
    }
    CallbackBridge(const CallbackBridge&) = delete;
    CallbackBridge& operator=(const CallbackBridge&) = delete;
    CallbackBridge(CallbackBridge&&) = delete;
    CallbackBridge& operator=(CallbackBridge&&) = delete;
    void activate() {
        const std::scoped_lock lock(slot_.mutex);
        if (slot_.active) {
            throw std::logic_error("A driver callback is already registered");
        }
        slot_.active = state_;
    }
    [[nodiscard]] static Result invoke(Args... args) noexcept {
        try {
            std::shared_ptr<State> state;
            {
                const std::scoped_lock lock(slot_.mutex);
                state = slot_.active;
            }
            if (!state) {
                return Rejected;
            }
            const std::scoped_lock lock(state->mutex);
            return state->enabled ? state->callback(std::forward<Args>(args)...) : Rejected;
        } catch (...) {
            // C callback boundary: the driver's failure value reports the error.
            return Rejected;
        }
    }
};
} // namespace stream
