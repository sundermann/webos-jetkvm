#include "callback_bridge.hpp"
#include "capture.hpp"
#include "cursor.hpp"
#include "file.hpp"
#include "input.hpp"
#include "resource.hpp"

#include <chrono>
#include <fcntl.h>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>

static_assert(!std::is_constructible_v<stream::Input, bool>);
static_assert(!std::is_constructible_v<stream::Cursor, bool>);
static_assert(
    std::is_same_v<decltype(&stream::make_capture), std::unique_ptr<stream::Capture> (*)()>);
using namespace std::chrono_literals;
void capture_backend_regressions(const char* module);

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
void bridge_lifetime() {
    using Bridge = stream::CallbackBridge<int(int), -1>;
    std::promise<void> entered;
    std::promise<void> finish;
    auto finished = finish.get_future().share();
    auto registration = std::make_unique<Bridge>([&](int value) {
        entered.set_value();
        finished.wait();
        return value + 1;
    });
    require(Bridge::invoke(0) == -1, "Unpublished callback ran");
    registration->activate();
    bool duplicate_rejected = false;
    try {
        Bridge duplicate([](int value) { return value; });
        duplicate.activate();
    } catch (const std::logic_error&) {
        duplicate_rejected = true;
    }
    require(duplicate_rejected, "Two callbacks acquired the same ABI slot");
    auto callback = std::async(std::launch::async, [] { return Bridge::invoke(41); });
    entered.get_future().wait();
    auto stopped = std::async(std::launch::async, [&] { registration.reset(); });
    const bool waited = stopped.wait_for(30ms) == std::future_status::timeout;
    finish.set_value();
    stopped.get();
    require(waited, "Callback owner was destroyed during an in-flight call");
    require(callback.get() == 42, "In-flight callback lost its state");
    require(Bridge::invoke(0) == -1, "Stopped callback was invoked");
    for (int iteration = 0; iteration < 50; ++iteration) {
        Bridge next([](int value) { return value * 2; });
        next.activate();
        require(Bridge::invoke(3) == 6, "Callback registration could not restart");
    }
    Bridge throwing([](int) -> int { throw std::runtime_error("driver consumer failed"); });
    throwing.activate();
    require(Bridge::invoke(1) == -1, "Exception escaped a C callback");
}
void descriptor_ownership() {
    std::optional<stream::UniqueFd> moved;
    int descriptor = -1;
    {
        stream::UniqueFd original(open("/dev/null", O_RDONLY));
        require(original.get() >= 0, "Cannot open descriptor fixture");
        descriptor = original.get();
        moved.emplace(std::move(original));
        require(moved->get() == descriptor, "Move lost descriptor ownership");
    }
    require(fcntl(descriptor, F_GETFD) >= 0, "Previous owner closed the moved descriptor");
    stream::UniqueFd replaced(open("/dev/null", O_RDONLY));
    const auto previous = replaced.get();
    replaced = std::move(*moved);
    moved.reset();
    require(fcntl(previous, F_GETFD) == -1, "Move assignment leaked a descriptor");
    require(fcntl(descriptor, F_GETFD) >= 0, "Move assignment duplicated ownership");
    require(replaced.close() == 0 && replaced.close() == 0, "Close was not idempotent");
    require(fcntl(descriptor, F_GETFD) == -1, "Owned descriptor was not closed");
}
void partial_acquisition() {
    struct Resource {
        unsigned closed = 0;
    } resource;
    const auto close = [](Resource* value) noexcept { ++value->closed; };
    bool failed = false;
    try {
        auto owner = stream::acquire_resource<Resource>(
            [&](Resource** output) noexcept {
                *output = &resource;
                return false;
            },
            close, "Injected registration failure");
    } catch (const std::runtime_error&) {
        failed = true;
    }
    require(failed && resource.closed == 1, "Partial registration leaked its handle");
    {
        auto owner = stream::acquire_resource<Resource>(
            [&](Resource** output) noexcept {
                *output = &resource;
                return true;
            },
            close, "Unexpected acquisition failure");
        require(owner.get() == &resource && resource.closed == 1,
            "Successful registration lost ownership");
    }
    require(resource.closed == 2, "Registered handle was not released exactly once");
}
} // namespace
int main(int argc, char** argv) try {
    require(argc == 2, "Expected a capture module path");
    bridge_lifetime();
    descriptor_ownership();
    partial_acquisition();
    capture_backend_regressions(argv[1]);
    std::cout << "Passed: callback lifetime, descriptor ownership, partial acquisition and capture "
                 "backend\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
