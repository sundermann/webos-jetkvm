#include "annexb.hpp"

#include <iostream>
#include <stdexcept>
using namespace stream;
static void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
static Bytes bytes(std::initializer_list<unsigned> data) {
    Bytes b;
    for (auto c : data) {
        b.push_back(std::byte(c));
    }
    return b;
}
int main() try {
    auto parameter = bytes({0, 0, 0, 1, 0x67, 0x42, 0, 0, 1, 0x68, 7});
    auto idr = bytes({0, 0, 1, 0x65, 7});
    require(split_annexb(parameter).size() == 2, "Split SPS and PPS NAL units");
    require(is_idr(idr) && !is_idr(parameter), "Detect IDR frames");
    ParameterSets cache;
    cache.observe(parameter);
    auto joined = cache.with_parameter_sets(idr);
    require(split_annexb(joined).size() == 3, "Prepend cached parameter sets");
    require(cache.with_parameter_sets(joined) == joined, "Preserve existing parameter sets");
    require(split_annexb(bytes({0, 0, 1})).empty(), "Ignore incomplete NAL unit");
    auto auds = bytes({0, 0, 0, 1, 9, 0xf0, 0, 0, 1, 0x65, 9, 0, 0, 1, 9, 0xf0, 0, 0, 1, 0x41, 8});
    const auto nals = split_annexb(auds);
    require(nals.size() == 4 && nals[0].type == 9 && nals[1].type == 5 && nals[2].type == 9 &&
                nals[3].type == 1,
        "Parse AUD, IDR and inter-frame NAL units");
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
