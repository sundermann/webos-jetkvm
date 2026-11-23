#include "capture_backend.hpp"

#include <stdexcept>

extern "C" stream::CaptureHardware* create_nyx_capture_backend() {
    throw std::runtime_error("Nyx fixture selected");
}
