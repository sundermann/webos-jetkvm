#include "annexb.hpp"

namespace stream {
std::vector<Nal> split_annexb(std::span<const std::byte> bytes) {
    std::vector<Nal> output;
    for (size_t i = 0; i + 3 <= bytes.size();) {
        size_t length = 0;
        if (bytes[i] == std::byte{0} && bytes[i + 1] == std::byte{0}) {
            if (bytes[i + 2] == std::byte{1}) {
                length = 3;
            } else if (i + 4 <= bytes.size() && bytes[i + 2] == std::byte{0} &&
                       bytes[i + 3] == std::byte{1}) {
                length = 4;
            }
        }
        if (length && i + length < bytes.size()) {
            if (!output.empty()) {
                output.back().end = i;
            }
            output.push_back({i, i + length, bytes.size(),
                static_cast<uint8_t>(std::to_integer<unsigned int>(bytes[i + length]) & 31u)});
            i += length + 1;
        } else {
            ++i;
        }
    }
    return output;
}
bool is_idr(std::span<const std::byte> bytes) {
    for (const auto& nal : split_annexb(bytes)) {
        if (nal.type == 5) {
            return true;
        }
    }
    return false;
}
void ParameterSets::observe(std::span<const std::byte> bytes) {
    for (const auto& nal : split_annexb(bytes)) {
        if (nal.type == 7) {
            sps_.assign(bytes.data() + nal.start, bytes.data() + nal.end);
        }
        if (nal.type == 8) {
            pps_.assign(bytes.data() + nal.start, bytes.data() + nal.end);
        }
    }
}
Bytes ParameterSets::with_parameter_sets(std::span<const std::byte> bytes) const {
    bool has_sps = false;
    bool has_pps = false;
    for (const auto& nal : split_annexb(bytes)) {
        has_sps |= nal.type == 7;
        has_pps |= nal.type == 8;
    }
    Bytes result;
    if (!has_sps) {
        result.insert(result.end(), sps_.begin(), sps_.end());
    }
    if (!has_pps) {
        result.insert(result.end(), pps_.begin(), pps_.end());
    }
    result.insert(result.end(), bytes.begin(), bytes.end());
    return result;
}
} // namespace stream
