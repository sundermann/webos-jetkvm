#pragma once

#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace stream {
// C APIs may return failure after assigning a partially initialized handle.
// Adopt that handle before checking success, so every exit runs its deleter.
template <class Resource, class Acquire, class Deleter>
[[nodiscard]] auto acquire_resource(Acquire acquire, Deleter deleter, const char* operation) {
    static_assert(std::is_nothrow_invocable_r_v<bool, Acquire, Resource**>);
    Resource* resource = nullptr;
    const bool acquired = acquire(&resource);
    std::unique_ptr<Resource, Deleter> owner(resource, std::move(deleter));
    if (!acquired || !owner) {
        throw std::runtime_error(operation);
    }
    return owner;
}
} // namespace stream
