#include "oracis/host/detail/sha1.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int fail(const char *message)
{
    std::cerr << "sha1_test: " << message << '\n';
    return 1;
}

} // namespace

int main()
{
    using oracis::host::detail::sha1_hex;

    if (sha1_hex({}) != "da39a3ee5e6b4b0d3255bfef95601890afd80709") {
        return fail("empty input digest mismatch");
    }

    const std::vector<uint8_t> abc{'a', 'b', 'c'};
    if (sha1_hex(abc) != "a9993e364706816aba3e25717850c26c9cd0d89d") {
        return fail("abc digest mismatch");
    }

    const std::vector<uint8_t> million_a(1000000u, 'a');
    if (sha1_hex(million_a) !=
        "34aa973cd4c4daa4f61eeb2bdbad27316534016f") {
        return fail("multi-block digest mismatch");
    }

    std::cout << "SHA-1 known vectors passed\n";
    return 0;
}
