#include "oracis/memory/nds_game_card_key2.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_game_card_key2_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using oracis::memory::NdsGameCardKey2;
    using oracis::memory::Status;

    constexpr uint64_t seed1 = UINT64_C(0x123456789A);
    constexpr uint64_t seed2 = UINT64_C(0x1ABCDEF012);
    constexpr uint8_t plaintext[16] = {
        0x00u,0x01u,0x02u,0x03u,0x04u,0x05u,0x06u,0x07u,
        0x10u,0x20u,0x30u,0x40u,0x50u,0x60u,0x70u,0x80u
    };
    constexpr uint8_t expected[16] = {
        0x25u,0xF2u,0xB3u,0xCCu,0x1Fu,0x1Eu,0xA2u,0x70u,
        0x2Du,0xB9u,0x53u,0x56u,0xA2u,0xCAu,0x3Au,0x33u
    };

    NdsGameCardKey2 stream;
    if (stream.apply_keystream(nullptr, 0u) != Status::Ok ||
        stream.apply_keystream(nullptr, 1u) != Status::InvalidRegion ||
        stream.set_seeds(NdsGameCardKey2::SeedMask + 1u, seed2) !=
            Status::InvalidRegion ||
        stream.initialized()) {
        return fail("uninitialized or out-of-range seed handling was not fail-closed");
    }

    if (stream.set_seeds(seed1, seed2) != Status::Ok ||
        stream.apply_keystream(nullptr, 1u) != Status::InvalidRegion) {
        return fail("could not initialize or validate KEY2 byte buffer");
    }

    std::array<uint8_t, 16> block{};
    for (uint32_t index = 0u; index < block.size(); ++index) {
        block[index] = plaintext[index];
    }

    /* Split at a non-word boundary to prove stream continuity. */
    if (stream.apply_keystream(block.data(), 7u) != Status::Ok ||
        stream.bytes_processed() != 7u ||
        stream.apply_keystream(block.data() + 7u, 9u) != Status::Ok ||
        stream.bytes_processed() != 16u) {
        return fail("KEY2 stream did not continue across split calls");
    }
    for (uint32_t index = 0u; index < block.size(); ++index) {
        if (block[index] != expected[index]) {
            return fail("KEY2 synthetic keystream vector mismatch");
        }
    }
    if (stream.state1() != UINT64_C(0x16811D70E) ||
        stream.state2() != UINT64_C(0x179ABB9DBD)) {
        return fail("KEY2 LFSR terminal states differed from the vector");
    }

    constexpr uint8_t header_seed_vector[16] = {
        0x46u,0x0Cu,0xCFu,0xFEu,0xA0u,0x41u,0x71u,0x55u,
        0x54u,0xE7u,0x43u,0xCBu,0x0Fu,0xC1u,0x18u,0xF2u
    };
    NdsGameCardKey2 from_header;
    if (from_header.initialize_from_random_seed(0x00123456u, 3u) != Status::Ok ||
        from_header.state1() != UINT64_C(0x46836A2C48) ||
        from_header.state2() != UINT64_C(0x506CECF09D) ||
        from_header.initialize_from_random_seed(0x01000000u, 3u) != Status::InvalidRegion ||
        from_header.initialize_from_random_seed(0x00123456u, 8u) != Status::InvalidRegion) {
        return fail("KEY2 protocol seed derivation or rejected-input checks failed");
    }
    std::array<uint8_t, 16> header_block{};
    for (uint32_t index = 0u; index < header_block.size(); ++index) {
        header_block[index] = plaintext[index];
    }
    if (from_header.apply_keystream(header_block.data(),
            static_cast<uint32_t>(header_block.size())) != Status::Ok) {
        return fail("KEY2 derived-seed stream could not be applied");
    }
    for (uint32_t index = 0u; index < header_block.size(); ++index) {
        if (header_block[index] != header_seed_vector[index]) {
            return fail("KEY2 header-derived synthetic vector mismatch");
        }
    }

    /* A fresh stream with the same explicit seeds reverses the XOR transformation. */
    NdsGameCardKey2 decrypt;
    if (decrypt.set_seeds(seed1, seed2) != Status::Ok ||
        decrypt.apply_keystream(block.data(), static_cast<uint32_t>(block.size())) !=
            Status::Ok) {
        return fail("could not reinitialize KEY2 stream for inverse test");
    }
    for (uint32_t index = 0u; index < block.size(); ++index) {
        if (block[index] != plaintext[index]) {
            return fail("KEY2 symmetric stream failed to restore plaintext");
        }
    }

    /* Reset must restore the original first byte and byte counter. */
    if (stream.set_seeds(seed1, seed2) != Status::Ok ||
        stream.bytes_processed() != 0u) {
        return fail("KEY2 seed reset did not reset stream position");
    }
    uint8_t first_byte = plaintext[0];
    if (stream.apply_keystream(&first_byte, 1u) != Status::Ok ||
        first_byte != expected[0]) {
        return fail("KEY2 stream reset did not reproduce its first byte");
    }

    std::puts("NDS KEY2 39-bit LFSRs, split-stream continuity and synthetic vector passed");
    return 0;
}
