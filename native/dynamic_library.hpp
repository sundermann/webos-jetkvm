#pragma once

#include <dlfcn.h>
#include <memory>
#include <stdexcept>
#include <string>

namespace stream {
class DynamicLibrary {
    struct Close {
        void operator()(void* handle) const noexcept {
            dlclose(handle);
        }
    };
    std::unique_ptr<void, Close> handle_;

  public:
    explicit DynamicLibrary(const char* name) : handle_(dlopen(name, RTLD_NOW | RTLD_LOCAL)) {
        if (!handle_) {
            throw std::runtime_error(dlerror());
        }
    }

    template <class Function> [[nodiscard]] Function symbol(const char* name) const {
        auto* address = dlsym(handle_.get(), name);
        if (!address) {
            throw std::runtime_error(std::string("Missing symbol ") + name);
        }
        // POSIX permits converting a dlsym result to its function-pointer type.
        return reinterpret_cast<Function>(address);
    }
};
} // namespace stream
