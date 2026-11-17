#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace stream {
using Bytes = std::vector<std::byte>;
struct Nal {
    size_t start = 0, payload = 0, end = 0;
    uint8_t type = 0;
};
[[nodiscard]] std::vector<Nal> split_annexb(std::span<const std::byte> bytes);
[[nodiscard]] bool is_idr(std::span<const std::byte> bytes);
class ParameterSets {
    Bytes sps_, pps_;

  public:
    void observe(std::span<const std::byte> bytes);
    Bytes with_parameter_sets(std::span<const std::byte> bytes) const;
};
} // namespace stream
