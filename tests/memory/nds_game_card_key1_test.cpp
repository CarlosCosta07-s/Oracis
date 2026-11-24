#include "oracis/memory/nds_game_card_key1.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

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

void store_u16_le(std::vector<uint8_t> &bytes, size_t offset, uint16_t value)
{
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1u] = static_cast<uint8_t>(value >> 8u);
}

void store_u32_le(std::vector<uint8_t> &bytes, size_t offset, uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; ++index) {
        bytes[offset + index] =
            static_cast<uint8_t>(value >> (index * 8u));
    }
}

uint16_t crc16_header(const uint8_t *bytes, size_t size)
{
    uint16_t crc = 0u;
    for (size_t index = 0u; index < size; ++index) {
        crc = static_cast<uint16_t>(crc ^ bytes[index]);
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            crc = static_cast<uint16_t>(
                (crc >> 1u) ^
                ((crc & 1u) != 0u ? UINT16_C(0xA001) : UINT16_C(0)));
        }
    }
    return crc;
}

uint16_t crc16_firmware(
    const std::vector<uint8_t> &arm9,
    const std::vector<uint8_t> &arm7)
{
    uint16_t crc = UINT16_C(0xFFFF);
    const auto update = [&crc](const std::vector<uint8_t> &input) {
        for (const uint8_t byte : input) {
            crc = static_cast<uint16_t>(crc ^ byte);
            for (uint32_t bit = 0u; bit < 8u; ++bit) {
                crc = static_cast<uint16_t>(
                    (crc >> 1u) ^
                    ((crc & 1u) != 0u ? UINT16_C(0xA001) : UINT16_C(0)));
            }
        }
    };
    update(arm9);
    update(arm7);
    return crc;
}

std::vector<uint8_t> make_lz77_literal_block(
    const std::array<uint8_t, 4> &decoded)
{
    return {
        UINT8_C(0x10), UINT8_C(4), UINT8_C(0), UINT8_C(0),
        UINT8_C(0), decoded[0], decoded[1], decoded[2], decoded[3]
    };
}

bool encrypt_firmware_stream(
    oracis::memory::NdsGameCardKey1 &cipher,
    const uint8_t *plain,
    size_t size,
    std::vector<uint8_t> &firmware,
    size_t offset)
{
    if (plain == nullptr || offset > firmware.size() ||
        size > firmware.size() - offset || (size & 7u) != 0u) {
        return false;
    }
    for (size_t position = 0u; position < size; position += 8u) {
        uint8_t block[8]{};
        for (size_t index = 0u; index < 8u; ++index) {
            block[index] = plain[position + index];
        }
        if (cipher.encrypt_block(block, 8u) !=
            oracis::memory::Status::Ok) {
            return false;
        }
        for (size_t index = 0u; index < 8u; ++index) {
            firmware[offset + position + index] = block[index];
        }
    }
    return true;
}

std::vector<uint8_t> make_synthetic_firmware(
    const std::array<uint8_t, oracis::memory::NdsGameCardKey1::KeyTableBytes>
        &table,
    const std::array<uint8_t, 4> &arm9_plain,
    const std::array<uint8_t, 4> &arm7_plain)
{
    using oracis::memory::NdsGameCardKey1;
    using oracis::memory::Status;
    constexpr uint32_t FirmwareId = UINT32_C(0x5043414D); /* "MACP" */
    const auto arm9_lz = make_lz77_literal_block(arm9_plain);
    const auto arm7_lz = make_lz77_literal_block(arm7_plain);
    std::vector<uint8_t> firmware(0x290u, 0u);

    store_u16_le(firmware, 0x0Cu, UINT16_C(4));       /* part 1 source: 0x200 */
    store_u16_le(firmware, 0x0Eu, UINT16_C(0x2024));  /* part 1 dest: 0x023FEE00 */
    store_u16_le(firmware, 0x10u, UINT16_C(0xA0));    /* part 2 source: 0x280 */
    store_u16_le(firmware, 0x12u, UINT16_C(0x90));    /* part 2 dest: 0x0380F700 */
    store_u16_le(firmware, 0x14u, UINT16_C(0x042D));
    store_u32_le(firmware, 0x08u, FirmwareId);
    store_u16_le(firmware, 0x2Cu, UINT16_C(0x0138));
    firmware[0x162u] = UINT8_C(0x1C);
    store_u16_le(
        firmware, 0x2Au,
        crc16_header(firmware.data() + 0x2Cu, 0x0137u));

    NdsGameCardKey1 cipher;
    if (cipher.load_key_table(table.data(), NdsGameCardKey1::KeyTableBytes) !=
            Status::Ok ||
        cipher.initialize(FirmwareId, 1u, 12u) != Status::Ok) {
        return {};
    }

    const std::array<uint8_t, 8> rom_info_plain = {
        UINT8_C(1), UINT8_C(2), UINT8_C(3), UINT8_C(4),
        UINT8_C(5), UINT8_C(6), UINT8_C(7), UINT8_C(8)
    };
    auto rom_info_cipher = rom_info_plain;
    if (cipher.encrypt_block(rom_info_cipher.data(), 8u) != Status::Ok) {
        return {};
    }
    for (size_t index = 0u; index < rom_info_cipher.size(); ++index) {
        firmware[0x18u + index] = rom_info_cipher[index];
    }

    if (cipher.initialize(FirmwareId, 2u, 12u) != Status::Ok) return {};
    std::vector<uint8_t> arm9_padded(16u, 0u);
    std::vector<uint8_t> arm7_padded(16u, 0u);
    std::copy(arm9_lz.begin(), arm9_lz.end(), arm9_padded.begin());
    std::copy(arm7_lz.begin(), arm7_lz.end(), arm7_padded.begin());
    if (!encrypt_firmware_stream(
            cipher, arm9_padded.data(), arm9_padded.size(), firmware, 0x200u) ||
        !encrypt_firmware_stream(
            cipher, arm7_padded.data(), arm7_padded.size(), firmware, 0x280u)) {
        return {};
    }

    const std::vector<uint8_t> arm9_vector(arm9_plain.begin(), arm9_plain.end());
    const std::vector<uint8_t> arm7_vector(arm7_plain.begin(), arm7_plain.end());
    store_u16_le(firmware, 0x06u, crc16_firmware(arm9_vector, arm7_vector));
    return firmware;
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

    /* Secure Area prefix is encrypted in two layers: ID at level 2,
     * then all 0x800 bytes at level 3. */
    constexpr uint32_t secure_area_game_code = UINT32_C(0x45555043);
    std::array<uint8_t,NdsGameCardKey1::SecureAreaEncryptedBytes>
        secure_area_plain{};
    for (uint32_t index = 0u; index < secure_area_plain.size(); ++index) {
        secure_area_plain[index] =
            static_cast<uint8_t>((index * 19u + 0x25u) & 0xFFu);
    }
    constexpr uint8_t secure_area_id[8] = {
        UINT8_C('e'), UINT8_C('n'), UINT8_C('c'), UINT8_C('r'),
        UINT8_C('y'), UINT8_C('O'), UINT8_C('b'), UINT8_C('j')
    };
    for (uint32_t index = 0u; index < 8u; ++index)
        secure_area_plain[index] = secure_area_id[index];

    auto secure_area_ciphertext = secure_area_plain;
    NdsGameCardKey1 secure_area_encryptor;
    if (secure_area_encryptor.load_key_table(
            table.data(), NdsGameCardKey1::KeyTableBytes) != Status::Ok ||
        secure_area_encryptor.initialize(
            secure_area_game_code, 3u, 8u) != Status::Ok) {
        return fail("could not initialize level-3 Secure Area test schedule");
    }
    for (uint32_t offset = 0u; offset < secure_area_ciphertext.size();
         offset += NdsGameCardKey1::BlockBytes) {
        if (secure_area_encryptor.encrypt_block(
                secure_area_ciphertext.data() + offset,
                NdsGameCardKey1::BlockBytes) != Status::Ok) {
            return fail("could not encrypt synthetic level-3 Secure Area");
        }
    }
    if (secure_area_encryptor.initialize(
            secure_area_game_code, 2u, 8u) != Status::Ok ||
        secure_area_encryptor.encrypt_block(
            secure_area_ciphertext.data(), NdsGameCardKey1::BlockBytes) != Status::Ok) {
        return fail("could not apply the additional level-2 Secure Area ID layer");
    }

    NdsGameCardKey1 secure_area_decryptor;
    if (secure_area_decryptor.load_key_table(
            table.data(), NdsGameCardKey1::KeyTableBytes) != Status::Ok ||
        secure_area_decryptor.initialize(
            secure_area_game_code, 2u, 8u) != Status::Ok) {
        return fail("could not initialize Secure Area decryptor");
    }
    bool secure_area_identifier_valid = false;
    if (secure_area_decryptor.decrypt_secure_area(
            secure_area_ciphertext.data(),
            static_cast<uint32_t>(secure_area_ciphertext.size()),
            secure_area_identifier_valid) != Status::Ok ||
        !secure_area_identifier_valid ||
        secure_area_decryptor.level() != 2u ||
        secure_area_decryptor.modulo_bytes() != 8u) {
        return fail("two-stage Secure Area KEY1 decryption or schedule restore failed");
    }
    for (uint32_t index = 0u; index < secure_area_plain.size(); ++index) {
        if (secure_area_ciphertext[index] != secure_area_plain[index]) {
            return fail("Secure Area KEY1 decryption did not restore all 0x800 bytes");
        }
    }

    /* A wrong identifier is reported to the caller without being confused
     * with a cryptographic/argument error. */
    auto invalid_secure_area = secure_area_plain;
    invalid_secure_area[0] = UINT8_C('X');
    auto invalid_secure_ciphertext = invalid_secure_area;
    if (secure_area_encryptor.initialize(
            secure_area_game_code, 3u, 8u) != Status::Ok) {
        return fail("could not restore level-3 schedule for invalid-ID test");
    }
    for (uint32_t offset = 0u; offset < invalid_secure_ciphertext.size();
         offset += NdsGameCardKey1::BlockBytes) {
        if (secure_area_encryptor.encrypt_block(
                invalid_secure_ciphertext.data() + offset,
                NdsGameCardKey1::BlockBytes) != Status::Ok) {
            return fail("could not encrypt invalid-ID Secure Area");
        }
    }
    if (secure_area_encryptor.initialize(
            secure_area_game_code, 2u, 8u) != Status::Ok ||
        secure_area_encryptor.encrypt_block(
            invalid_secure_ciphertext.data(), NdsGameCardKey1::BlockBytes) != Status::Ok ||
        secure_area_decryptor.decrypt_secure_area(
            invalid_secure_ciphertext.data(),
            static_cast<uint32_t>(invalid_secure_ciphertext.size()),
            secure_area_identifier_valid) != Status::Ok ||
        secure_area_identifier_valid) {
        return fail("Secure Area decryptor accepted an invalid encrypted identifier");
    }
    if (secure_area_decryptor.decrypt_secure_area(
            invalid_secure_ciphertext.data(),
            static_cast<uint32_t>(invalid_secure_ciphertext.size() - 1u),
            secure_area_identifier_valid) != Status::InvalidWidth ||
        secure_area_decryptor.decrypt_secure_area(
            nullptr, NdsGameCardKey1::SecureAreaEncryptedBytes,
            secure_area_identifier_valid) != Status::InvalidRegion) {
        return fail("Secure Area decryptor accepted invalid buffer arguments");
    }


    /* Firmware IPL: level-1 rominfo and level-2 encrypted LZ77 code parts. */
    constexpr std::array<uint8_t, 4> firmware_arm9_plain = {
        UINT8_C(0xA1), UINT8_C(0xA2), UINT8_C(0xA3), UINT8_C(0xA4)
    };
    constexpr std::array<uint8_t, 4> firmware_arm7_plain = {
        UINT8_C(0xB1), UINT8_C(0xB2), UINT8_C(0xB3), UINT8_C(0xB4)
    };
    auto synthetic_firmware = make_synthetic_firmware(
        table, firmware_arm9_plain, firmware_arm7_plain);
    if (synthetic_firmware.size() != 0x290u ||
        key1.load_key_table(table.data(), NdsGameCardKey1::KeyTableBytes) !=
            Status::Ok ||
        key1.initialize(UINT32_C(0x45555043), 1u, 8u) != Status::Ok) {
        return fail("could not prepare synthetic encrypted firmware input");
    }

    oracis::memory::NdsFirmwareBootCode firmware_boot{};
    if (key1.decrypt_firmware_boot_code(
            synthetic_firmware.data(), synthetic_firmware.size(),
            firmware_boot) != Status::Ok ||
        firmware_boot.arm9_load_address != UINT32_C(0x023FEE00) ||
        firmware_boot.arm7_load_address != UINT32_C(0x0380F700) ||
        firmware_boot.arm9_bytes != std::vector<uint8_t>(
            firmware_arm9_plain.begin(), firmware_arm9_plain.end()) ||
        firmware_boot.arm7_bytes != std::vector<uint8_t>(
            firmware_arm7_plain.begin(), firmware_arm7_plain.end()) ||
        firmware_boot.computed_crc16 != firmware_boot.expected_crc16 ||
        firmware_boot.decrypted_rom_info != std::array<uint8_t, 8>{
            1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u} ||
        key1.level() != 1u || key1.modulo_bytes() != 8u ||
        key1.id_code() != UINT32_C(0x45555043)) {
        return fail("firmware IPL decryption, LZ77, CRC, destinations or key restoration failed");
    }

    auto corrupted_firmware = synthetic_firmware;
    corrupted_firmware[0x06u] ^= UINT8_C(0x80);
    auto preserved_output = firmware_boot;
    if (key1.decrypt_firmware_boot_code(
            corrupted_firmware.data(), corrupted_firmware.size(),
            preserved_output) != Status::IdentityMismatch ||
        preserved_output.arm9_bytes != firmware_boot.arm9_bytes ||
        preserved_output.arm7_bytes != firmware_boot.arm7_bytes) {
        return fail("firmware IPL accepted a corrupted combined CRC or partially replaced output");
    }

    auto corrupted_header = synthetic_firmware;
    corrupted_header[0x30u] ^= UINT8_C(0x01);
    if (key1.decrypt_firmware_boot_code(
            corrupted_header.data(), corrupted_header.size(),
            preserved_output) != Status::IdentityMismatch ||
        preserved_output.arm9_bytes != firmware_boot.arm9_bytes) {
        return fail("firmware IPL accepted a corrupt remaining-header CRC");
    }

    auto unsupported_firmware = synthetic_firmware;
    unsupported_firmware[0x08u] = UINT8_C('X');
    if (key1.decrypt_firmware_boot_code(
            unsupported_firmware.data(), unsupported_firmware.size(),
            preserved_output) != Status::Unsupported ||
        key1.decrypt_firmware_boot_code(
            synthetic_firmware.data(), 0x1FFu,
            preserved_output) != Status::InvalidRegion) {
        return fail("firmware IPL accepted an unsupported ID or truncated flash");
    }

    std::puts("NDS KEY1 and firmware IPL decryption/LZ77/CRC tests passed");
    key1.reset();
    if (!key1.table_loaded() || key1.initialized() || key1.id_code() != 0u ||
        key1.level() != 0u || key1.modulo_bytes() != 0u ||
        key1.initialize(UINT32_C(0x45555043), 2u, 8u) != Status::Ok ||
        !key1.initialized()) {
        return fail("KEY1 reset did not preserve the base table for a fresh cartridge");
    }

    return 0;
}
