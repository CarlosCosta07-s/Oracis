#include "oracis/host/detail/sha1.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace oracis::host::detail {
namespace {

uint32_t rotate_left(uint32_t value, unsigned amount) noexcept
{
    return (value << amount) | (value >> (32u - amount));
}

std::array<uint8_t, 20> sha1_digest(const std::vector<uint8_t> &input)
{
    const uint64_t bit_length = static_cast<uint64_t>(input.size()) * 8u;
    const std::size_t padded_size =
        ((input.size() + 9u + 63u) / 64u) * 64u;
    std::vector<uint8_t> padded(padded_size, 0u);
    for (std::size_t index = 0u; index < input.size(); ++index) {
        padded[index] = input[index];
    }
    padded[input.size()] = 0x80u;
    for (unsigned index = 0u; index < 8u; ++index) {
        padded[padded_size - 1u - index] =
            static_cast<uint8_t>(bit_length >> (index * 8u));
    }

    uint32_t h0 = 0x67452301u;
    uint32_t h1 = 0xEFCDAB89u;
    uint32_t h2 = 0x98BADCFEu;
    uint32_t h3 = 0x10325476u;
    uint32_t h4 = 0xC3D2E1F0u;

    for (std::size_t offset = 0u; offset < padded.size(); offset += 64u) {
        uint32_t words[80]{};
        for (unsigned index = 0u; index < 16u; ++index) {
            const std::size_t byte = offset + index * 4u;
            words[index] =
                (static_cast<uint32_t>(padded[byte]) << 24u) |
                (static_cast<uint32_t>(padded[byte + 1u]) << 16u) |
                (static_cast<uint32_t>(padded[byte + 2u]) << 8u) |
                static_cast<uint32_t>(padded[byte + 3u]);
        }
        for (unsigned index = 16u; index < 80u; ++index) {
            words[index] = rotate_left(
                words[index - 3u] ^ words[index - 8u] ^
                words[index - 14u] ^ words[index - 16u], 1u);
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;
        for (unsigned index = 0u; index < 80u; ++index) {
            uint32_t function = 0u;
            uint32_t constant = 0u;
            if (index < 20u) {
                function = (b & c) | ((~b) & d);
                constant = 0x5A827999u;
            } else if (index < 40u) {
                function = b ^ c ^ d;
                constant = 0x6ED9EBA1u;
            } else if (index < 60u) {
                function = (b & c) | (b & d) | (c & d);
                constant = 0x8F1BBCDCu;
            } else {
                function = b ^ c ^ d;
                constant = 0xCA62C1D6u;
            }
            const uint32_t temporary =
                rotate_left(a, 5u) + function + e + constant + words[index];
            e = d;
            d = c;
            c = rotate_left(b, 30u);
            b = a;
            a = temporary;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    const uint32_t state[5]{h0, h1, h2, h3, h4};
    std::array<uint8_t, 20> digest{};
    for (unsigned word = 0u; word < 5u; ++word) {
        for (unsigned byte = 0u; byte < 4u; ++byte) {
            digest[word * 4u + byte] =
                static_cast<uint8_t>(state[word] >> (24u - byte * 8u));
        }
    }
    return digest;
}

} // namespace

std::string sha1_hex(const std::vector<uint8_t> &bytes)
{
    static constexpr char kHex[] = "0123456789abcdef";
    const auto digest = sha1_digest(bytes);
    std::string result;
    result.reserve(40u);
    for (const uint8_t byte : digest) {
        result.push_back(kHex[byte >> 4u]);
        result.push_back(kHex[byte & 0x0fu]);
    }
    return result;
}

} // namespace oracis::host::detail
