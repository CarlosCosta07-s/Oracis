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

std::array<uint8_t, 8> make_key1_command(
    uint8_t opcode, uint16_t parameter16,
    uint32_t following24, uint32_t counter20)
{
    return {{
        static_cast<uint8_t>((opcode << 4u) | ((parameter16 >> 12u) & 0x0Fu)),
        static_cast<uint8_t>(parameter16 >> 4u),
        static_cast<uint8_t>(((parameter16 & 0x0Fu) << 4u) |
                             ((following24 >> 20u) & 0x0Fu)),
        static_cast<uint8_t>(following24 >> 12u),
        static_cast<uint8_t>(following24 >> 4u),
        static_cast<uint8_t>(((following24 & 0x0Fu) << 4u) |
                             ((counter20 >> 16u) & 0x0Fu)),
        static_cast<uint8_t>(counter20 >> 8u),
        static_cast<uint8_t>(counter20)
    }};
}

} // namespace

int main()
{
    using oracis::memory::NdsGameCardKey1;
    using oracis::memory::NdsGameCardKey1Command;
    using oracis::memory::NdsGameCardKey1CommandKind;
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

    NdsGameCardKey1Command decoded{};
    if (key1.decode_command(nullptr, NdsGameCardKey1::BlockBytes,
            UINT32_C(0x00054322), UINT16_C(0x0ABC), UINT16_C(0x0123),
            decoded) != Status::InvalidRegion) {
        return fail("KEY1 command decoder accepted a null command buffer");
    }
    constexpr uint16_t protocol_i = UINT16_C(0x0ABC);
    constexpr uint16_t protocol_j = UINT16_C(0x0123);
    constexpr uint32_t activation_counter = UINT32_C(0x54321);
    constexpr uint32_t first_command_counter = activation_counter + 1u;

    auto activate_key2 = make_key1_command(
        0x04u, UINT16_C(0x1234), UINT32_C(0xABCDEF),
        first_command_counter);
    if (key1.encrypt_command(activate_key2.data(),
            static_cast<uint32_t>(activate_key2.size())) != Status::Ok) {
        return fail("could not encrypt synthetic KEY1 command 0x40");
    }

    if (key1.decode_command(activate_key2.data(),
            static_cast<uint32_t>(activate_key2.size()),
            first_command_counter, protocol_i, protocol_j, decoded) != Status::Ok ||
        decoded.kind != NdsGameCardKey1CommandKind::ActivateKey2 ||
        decoded.opcode != 0x04u ||
        decoded.parameter16 != UINT16_C(0x1234) ||
        decoded.key2_seed24 != UINT32_C(0xABCDEF) ||
        decoded.counter20 != first_command_counter) {
        return fail("KEY1 command 0x40 fields were not decoded correctly");
    }

    auto secure_block = make_key1_command(
        0x02u, UINT16_C(0x0006),
        (static_cast<uint32_t>(protocol_i) << 12u) | protocol_j,
        first_command_counter + 1u);
    if (key1.encrypt_command(secure_block.data(),
            static_cast<uint32_t>(secure_block.size())) != Status::Ok ||
        key1.decode_command(secure_block.data(),
            static_cast<uint32_t>(secure_block.size()),
            first_command_counter + 1u, protocol_i, protocol_j, decoded) != Status::Ok ||
        decoded.kind != NdsGameCardKey1CommandKind::ReadSecureAreaBlock ||
        decoded.secure_area_block != 6u ||
        decoded.random_i != protocol_i || decoded.random_j != protocol_j) {
        return fail("KEY1 secure-area block command parameters were not validated");
    }

    auto wrong_randoms = make_key1_command(
        0x02u, UINT16_C(0x0006), UINT32_C(0xABC124),
        first_command_counter + 2u);
    if (key1.encrypt_command(wrong_randoms.data(),
            static_cast<uint32_t>(wrong_randoms.size())) != Status::Ok ||
        key1.decode_command(wrong_randoms.data(),
            static_cast<uint32_t>(wrong_randoms.size()),
            first_command_counter + 2u, protocol_i, protocol_j, decoded) !=
            Status::PermissionDenied) {
        return fail("KEY1 accepted secure-area command with mismatched i/j values");
    }

    auto wrong_counter = make_key1_command(
        0x04u, 0u, UINT32_C(0x000001), first_command_counter + 3u);
    if (key1.encrypt_command(wrong_counter.data(),
            static_cast<uint32_t>(wrong_counter.size())) != Status::Ok ||
        key1.decode_command(wrong_counter.data(),
            static_cast<uint32_t>(wrong_counter.size()),
            first_command_counter + 4u, protocol_i, protocol_j, decoded) !=
            Status::PermissionDenied) {
        return fail("KEY1 accepted a command with an unexpected rolling counter");
    }

    auto invalid_block_command = make_key1_command(
        0x02u, UINT16_C(0x0003),
        (static_cast<uint32_t>(protocol_i) << 12u) | protocol_j,
        first_command_counter + 5u);
    if (key1.encrypt_command(invalid_block_command.data(),
            static_cast<uint32_t>(invalid_block_command.size())) != Status::Ok ||
        key1.decode_command(invalid_block_command.data(),
            static_cast<uint32_t>(invalid_block_command.size()),
            first_command_counter + 5u, protocol_i, protocol_j, decoded) !=
            Status::PermissionDenied) {
        return fail("KEY1 accepted a secure-area block outside 0004h..0007h");
    }

    auto unknown_command = make_key1_command(
        0x03u, 0u, 0u, first_command_counter + 6u);
    if (key1.encrypt_command(unknown_command.data(),
            static_cast<uint32_t>(unknown_command.size())) != Status::Ok ||
        key1.decode_command(unknown_command.data(),
            static_cast<uint32_t>(unknown_command.size()),
            first_command_counter + 6u, protocol_i, protocol_j, decoded) !=
            Status::PermissionDenied) {
        return fail("KEY1 accepted an unsupported encrypted command opcode");
    }
    if (key1.decode_command(unknown_command.data(), 7u,
            first_command_counter + 6u, protocol_i, protocol_j, decoded) !=
            Status::InvalidWidth ||
        key1.decode_command(unknown_command.data(),
            static_cast<uint32_t>(unknown_command.size()),
            UINT32_C(0x00100000), protocol_i, protocol_j, decoded) !=
            Status::InvalidRegion) {
        return fail("KEY1 command decoder accepted an invalid size or counter");
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
