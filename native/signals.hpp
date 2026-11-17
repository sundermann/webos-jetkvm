#pragma once

#include <csignal>
#include <pthread.h>
#include <system_error>

namespace stream {
// Construct before workers: they inherit the mask, so termination is handled
// synchronously on the main thread without mutable globals or signal handlers.
class TerminationSignals {
    sigset_t signals_{};
    sigset_t previous_{};

  public:
    TerminationSignals() {
        sigemptyset(&signals_);
        sigaddset(&signals_, SIGINT);
        sigaddset(&signals_, SIGTERM);
        const auto result = pthread_sigmask(SIG_BLOCK, &signals_, &previous_);
        if (result != 0) {
            throw std::system_error(result, std::generic_category(), "Block termination signals");
        }
    }
    ~TerminationSignals() {
        pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }
    TerminationSignals(const TerminationSignals&) = delete;
    TerminationSignals& operator=(const TerminationSignals&) = delete;
    TerminationSignals(TerminationSignals&&) = delete;
    TerminationSignals& operator=(TerminationSignals&&) = delete;
    void wait() const {
        int signal = 0;
        const auto result = sigwait(&signals_, &signal);
        if (result != 0) {
            throw std::system_error(result, std::generic_category(), "Wait for termination");
        }
    }
};
} // namespace stream
