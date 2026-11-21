#include "oracis/memory/nds_game_card_key1.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_game_card_key1_test: %s\n", message);
    return 1;
}

std::array<uint8_t, oracis::memory::NdsGameCardKey1::KeyTableBytes>
synthetic_key_table()
{
    std::array<uint8_t, oracis::memory::NdsGameCardKey1::KeyTableBytes> bytes{};
    for (uint32_t index = 0u; index < bytes.size(); ++index) {
        bytes[index] = static_cast<uint8_t>((index * 37u + 11u) & 0xFFu);
    }
    return bytes;
}

} // namespace

int main()
{
    using oracis::memory::NdsGameCardKey1;
    using oracis::memory::Status;

    NdsGameCardKey1 key1;
    const auto table = synthetic_key_table();
    if (key1.initialize(UINT32_C(0x45555043), 2u, 8u) != Status::Unmapped ||
        key1.load_key_table(nullptr, NdsGameCardKey1::KeyTableBytes) !=
            Status::InvalidRegion ||
        key1.load_key_table(table.data(), NdsGameCardKey1::KeyTableBytes - 1u) !=
            Status::InvalidRegion) {
        return fail("missing or malformed external key table was accepted");
    }

    if (key1.load_key_table(table.data(), NdsGameCardKey1::KeyTableBytes) !=
        Status::Ok) {
        return fail("synthetic external key table was rejected");
    }
    uint8_t invalid_block[NdsGameCardKey1::BlockBytes]{};
    if (key1.encrypt_block(nullptr, NdsGameCardKey1::BlockBytes) !=
            Status::InvalidRegion ||
        key1.encrypt_block(invalid_block, 7u) != Status::InvalidWidth ||
        key1.encrypt_block(invalid_block, NdsGameCardKey1::BlockBytes) !=
            Status::PermissionDenied ||
        key1.encrypt_command(nullptr, NdsGameCardKey1::BlockBytes) !=
            Status::InvalidRegion ||
        key1.encrypt_command(invalid_block, 7u) != Status::InvalidWidth ||
        key1.encrypt_command(invalid_block, NdsGameCardKey1::BlockBytes) !=
            Status::PermissionDenied ||
        key1.initialize(UINT32_C(0x45555043), 0u, 8u) != Status::InvalidRegion ||
        key1.initialize(UINT32_C(0x45555043), 2u, 4u) != Status::InvalidRegion) {
        return fail("KEY1 rejected-input validation was not fail-closed");
    }

    constexpr uint8_t plaintext[8] = {
        UINT8_C(0x67), UINT8_C(0x45), UINT8_C(0x23), UINT8_C(0x01),
        UINT8_C(0xEF), UINT8_C(0xCD), UINT8_C(0xAB), UINT8_C(0x89)
    };
    constexpr uint8_t level2_expected[8] = {
        UINT8_C(0x4E), UINT8_C(0x30), UINT8_C(0xDB), UINT8_C(0x9B),
        UINT8_C(0x0D), UINT8_C(0x05), UINT8_C(0x1C), UINT8_C(0x60)
    };
    constexpr uint8_t level1_expected[8] = {
        UINT8_C(0x60), UINT8_C(0x21), UINT8_C(0xCE), UINT8_C(0x6E),
        UINT8_C(0xF0), UINT8_C(0x4F), UINT8_C(0xBB), UINT8_C(0x9A)
    };
    constexpr uint8_t level3_expected[8] = {
        UINT8_C(0x36), UINT8_C(0x48), UINT8_C(0xAD), UINT8_C(0xD8),
        UINT8_C(0x6A), UINT8_C(0x9B), UINT8_C(0xD3), UINT8_C(0xF1)
    };
    constexpr uint8_t modulo12_expected[8] = {
        UINT8_C(0x8E), UINT8_C(0x21), UINT8_C(0x0F), UINT8_C(0xE2),
        UINT8_C(0xA0), UINT8_C(0xDA), UINT8_C(0x12), UINT8_C(0x8D)
    };

    const auto run_vector = [&key1](uint32_t level, uint32_t modulo,
                                    const uint8_t *expected) -> bool {
        uint8_t block[8] = {
            UINT8_C(0x67), UINT8_C(0x45), UINT8_C(0x23), UINT8_C(0x01),
            UINT8_C(0xEF), UINT8_C(0xCD), UINT8_C(0xAB), UINT8_C(0x89)
        };
        if (key1.initialize(UINT32_C(0x45555043), level, modulo) != Status::Ok ||
            key1.encrypt_block(block, sizeof(block)) != Status::Ok) {
            return false;
        }
        for (uint32_t index = 0u; index < sizeof(block); ++index) {
            if (block[index] != expected[index]) return false;
        }
        if (key1.decrypt_block(block, sizeof(block)) != Status::Ok) {
            return false;
        }
        constexpr uint8_t source[8] = {
            UINT8_C(0x67), UINT8_C(0x45), UINT8_C(0x23), UINT8_C(0x01),
            UINT8_C(0xEF), UINT8_C(0xCD), UINT8_C(0xAB), UINT8_C(0x89)
        };
        for (uint32_t index = 0u; index < sizeof(block); ++index) {
            if (block[index] != source[index]) return false;
        }
        return true;
    };

    if (!run_vector(1u, 8u, level1_expected) ||
        !run_vector(2u, 8u, level2_expected) ||
        !run_vector(3u, 8u, level3_expected) ||
        !run_vector(1u, 12u, modulo12_expected) ||
        key1.id_code() != UINT32_C(0x45555043) ||
        key1.level() != 1u || key1.modulo_bytes() != 12u) {
        return fail("KEY1 schedule/encryption/decryption vector mismatch");
    }

    if (key1.initialize(UINT32_C(0x45555043), 2u, 8u) != Status::Ok) {
        return fail("could not restore level-2 KEY1 schedule for command-order vector");
    }
    uint8_t wire_command[8] = {
        plaintext[7], plaintext[6], plaintext[5], plaintext[4],
        plaintext[3], plaintext[2], plaintext[1], plaintext[0]
    };
    constexpr uint8_t encrypted_wire_expected[8] = {
        UINT8_C(0x60), UINT8_C(0x1C), UINT8_C(0x05), UINT8_C(0x0D),
        UINT8_C(0x9B), UINT8_C(0xDB), UINT8_C(0x30), UINT8_C(0x4E)
    };
    if (key1.encrypt_command(wire_command, sizeof(wire_command)) != Status::Ok) {
        return fail("KEY1 command encryption rejected a valid wire-order block");
    }
    for (uint32_t index = 0u; index < sizeof(wire_command); ++index) {
        if (wire_command[index] != encrypted_wire_expected[index]) {
            return fail("KEY1 command encryption used the wrong byte order");
        }
    }
    if (key1.decrypt_command(wire_command, sizeof(wire_command)) != Status::Ok) {
        return fail("KEY1 command decryption rejected a valid wire-order block");
    }
    for (uint32_t index = 0u; index < sizeof(wire_command); ++index) {
        if (wire_command[index] != plaintext[7u - index]) {
            return fail("KEY1 command decrypt did not restore MSB-first wire bytes");
        }
    }

    std::puts("NDS KEY1 table loading, code levels, block vectors and command byte order passed");
    key1.reset();
    if (!key1.table_loaded() || key1.initialized() || key1.id_code() != 0u ||
        key1.level() != 0u || key1.modulo_bytes() != 0u ||
        key1.initialize(UINT32_C(0x45555043), 2u, 8u) != Status::Ok ||
        !key1.initialized()) {
        return fail("KEY1 reset did not preserve the base table for a fresh cartridge");
    }

    return 0;
}
