#include "oracis/host/runtime_bridge.h"
#include "oracis/memory/nds_game_card_key1.hpp"
#include "oracis/recomp/runtime_arm.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

constexpr uint32_t kArm9RomOffset = UINT32_C(0x4000);
constexpr uint32_t kArm9LoadAddress = UINT32_C(0x02000000);
constexpr uint32_t kArm9EntryPoint = UINT32_C(0x02000800);
constexpr uint32_t kArm9Size = UINT32_C(0x1000);
constexpr uint32_t kArm7RomOffset = UINT32_C(0x5000);
constexpr uint32_t kArm7LoadAddress = UINT32_C(0x02381000);
constexpr uint32_t kArm7Size = UINT32_C(4);
constexpr uint32_t kGameCode = UINT32_C(0x45555043);

int fail(const char *message)
{
    std::cerr << "game_card_boot_loader_test: " << message << '\n';
    return 1;
}

void write_u32_le(std::vector<uint8_t> &bytes, size_t offset, uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; ++index) {
        bytes[offset + index] =
            static_cast<uint8_t>(value >> (index * 8u));
    }
}

uint32_t read_u32_le(const uint8_t *bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8u) |
        (static_cast<uint32_t>(bytes[2]) << 16u) |
        (static_cast<uint32_t>(bytes[3]) << 24u);
}

uint16_t read_u16_le(const uint8_t *bytes)
{
    return static_cast<uint16_t>(
        static_cast<uint16_t>(bytes[0]) |
        (static_cast<uint16_t>(bytes[1]) << 8u));
}

void store_u16_le(std::vector<uint8_t> &bytes, size_t offset, uint16_t value)
{
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1u] = static_cast<uint8_t>(value >> 8u);
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
    const std::array<uint8_t, 4> &arm9,
    const std::array<uint8_t, 4> &arm7)
{
    uint16_t crc = UINT16_C(0xFFFF);
    const auto update = [&crc](const std::array<uint8_t, 4> &bytes) {
        for (const uint8_t byte : bytes) {
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
        std::array<uint8_t, 8> block{};
        std::copy_n(plain + position, block.size(), block.begin());
        if (cipher.encrypt_block(
                block.data(), static_cast<uint32_t>(block.size())) !=
            oracis::memory::Status::Ok) {
            return false;
        }
        std::copy(block.begin(), block.end(), firmware.begin() +
            static_cast<std::ptrdiff_t>(offset + position));
    }
    return true;
}

std::vector<uint8_t> make_synthetic_firmware(
    const uint8_t *key_table,
    uint32_t key_table_size)
{
    using oracis::memory::NdsGameCardKey1;
    using oracis::memory::Status;
    constexpr uint32_t FirmwareId = UINT32_C(0x5043414D); /* MACP */
    constexpr std::array<uint8_t, 4> Arm9Plain = {
        UINT8_C(0xA1), UINT8_C(0xA2), UINT8_C(0xA3), UINT8_C(0xA4)
    };
    constexpr std::array<uint8_t, 4> Arm7Plain = {
        UINT8_C(0xB1), UINT8_C(0xB2), UINT8_C(0xB3), UINT8_C(0xB4)
    };

    if (key_table == nullptr ||
        key_table_size != NdsGameCardKey1::KeyTableBytes) {
        return {};
    }

    const auto arm9_lz = make_lz77_literal_block(Arm9Plain);
    const auto arm7_lz = make_lz77_literal_block(Arm7Plain);
    std::vector<uint8_t> firmware(0x290u, 0u);
    store_u16_le(firmware, 0x0Cu, UINT16_C(4));       /* source 0x200 */
    store_u16_le(firmware, 0x0Eu, UINT16_C(0x8024));  /* ARM9: 0x023FEE00 */
    store_u16_le(firmware, 0x10u, UINT16_C(0xA0));    /* source 0x280 */
    store_u16_le(firmware, 0x12u, UINT16_C(0x90));    /* ARM7: 0x0380F700 */
    store_u16_le(firmware, 0x14u, UINT16_C(0x042D));
    write_u32_le(firmware, 0x08u, FirmwareId);
    store_u16_le(firmware, 0x2Cu, UINT16_C(0x0138));
    firmware[0x162u] = UINT8_C(0x1C);
    store_u16_le(
        firmware, 0x2Au,
        crc16_header(firmware.data() + 0x2Cu, 0x0137u));

    NdsGameCardKey1 cipher;
    if (cipher.load_key_table(key_table, key_table_size) != Status::Ok ||
        cipher.initialize(FirmwareId, 1u, 12u) != Status::Ok) {
        return {};
    }

    std::array<uint8_t, 8> rom_info = {
        UINT8_C(1), UINT8_C(2), UINT8_C(3), UINT8_C(4),
        UINT8_C(5), UINT8_C(6), UINT8_C(7), UINT8_C(8)
    };
    if (cipher.encrypt_block(rom_info.data(), 8u) != Status::Ok) {
        return {};
    }
    std::copy(rom_info.begin(), rom_info.end(), firmware.begin() + 0x18u);

    if (cipher.initialize(FirmwareId, 2u, 12u) != Status::Ok) {
        return {};
    }
    std::array<uint8_t, 16> arm9_padded{};
    std::array<uint8_t, 16> arm7_padded{};
    std::copy(arm9_lz.begin(), arm9_lz.end(), arm9_padded.begin());
    std::copy(arm7_lz.begin(), arm7_lz.end(), arm7_padded.begin());
    if (!encrypt_firmware_stream(
            cipher, arm9_padded.data(), arm9_padded.size(), firmware, 0x200u) ||
        !encrypt_firmware_stream(
            cipher, arm7_padded.data(), arm7_padded.size(), firmware, 0x280u)) {
        return {};
    }

    store_u16_le(firmware, 0x06u, crc16_firmware(Arm9Plain, Arm7Plain));
    return firmware;
}

struct Fixture {
    std::vector<uint8_t> image;
    std::vector<uint8_t> arm7_bios;
};

bool make_fixture(Fixture &fixture)
{
    using oracis::memory::NdsGameCardKey1;

    fixture.image.assign(kArm7RomOffset + kArm7Size, 0u);
    fixture.arm7_bios.assign(NdsGameCardKey1::Arm7BiosBytes, 0u);

    fixture.image[0x0Cu] = static_cast<uint8_t>('C');
    fixture.image[0x0Du] = static_cast<uint8_t>('P');
    fixture.image[0x0Eu] = static_cast<uint8_t>('U');
    fixture.image[0x0Fu] = static_cast<uint8_t>('E');

    write_u32_le(fixture.image, 0x20u, kArm9RomOffset);
    write_u32_le(fixture.image, 0x24u, kArm9EntryPoint);
    write_u32_le(fixture.image, 0x28u, kArm9LoadAddress);
    write_u32_le(fixture.image, 0x2Cu, kArm9Size);

    write_u32_le(fixture.image, 0x30u, kArm7RomOffset);
    write_u32_le(fixture.image, 0x34u, kArm7LoadAddress);
    write_u32_le(fixture.image, 0x38u, kArm7LoadAddress);
    write_u32_le(fixture.image, 0x3Cu, kArm7Size);

    for (uint32_t index = 0u; index < NdsGameCardKey1::KeyTableBytes; ++index) {
        fixture.arm7_bios[NdsGameCardKey1::Arm7BiosKeyTableOffset + index] =
            static_cast<uint8_t>((index * 37u + 11u) & 0xFFu);
    }

    for (uint32_t index = 0u; index < kArm9Size; ++index) {
        fixture.image[kArm9RomOffset + index] =
            static_cast<uint8_t>((index * 19u + 0x25u) & 0xFFu);
    }

    std::array<uint8_t, NdsGameCardKey1::SecureAreaEncryptedBytes>
        encrypted_prefix{};
    std::copy_n(
        fixture.image.data() + kArm9RomOffset,
        encrypted_prefix.size(), encrypted_prefix.data());
    constexpr uint8_t identifier[8] = {
        'e', 'n', 'c', 'r', 'y', 'O', 'b', 'j'
    };
    std::copy(identifier, identifier + 8, encrypted_prefix.begin());

    NdsGameCardKey1 cipher;
    const uint8_t *key_table = fixture.arm7_bios.data() +
        NdsGameCardKey1::Arm7BiosKeyTableOffset;
    if (cipher.load_key_table(key_table, NdsGameCardKey1::KeyTableBytes) !=
            oracis::memory::Status::Ok ||
        cipher.initialize(kGameCode, 3u, 8u) !=
            oracis::memory::Status::Ok) {
        return false;
    }
    for (uint32_t offset = 0u; offset < encrypted_prefix.size();
         offset += NdsGameCardKey1::BlockBytes) {
        if (cipher.encrypt_block(
                encrypted_prefix.data() + offset,
                NdsGameCardKey1::BlockBytes) !=
            oracis::memory::Status::Ok) {
            return false;
        }
    }
    if (cipher.initialize(kGameCode, 2u, 8u) !=
            oracis::memory::Status::Ok ||
        cipher.encrypt_block(
            encrypted_prefix.data(), NdsGameCardKey1::BlockBytes) !=
            oracis::memory::Status::Ok) {
        return false;
    }
    std::copy(
        encrypted_prefix.begin(), encrypted_prefix.end(),
        fixture.image.begin() + kArm9RomOffset);

    fixture.image[kArm7RomOffset + 0u] = UINT8_C(0x00);
    fixture.image[kArm7RomOffset + 1u] = UINT8_C(0x00);
    fixture.image[kArm7RomOffset + 2u] = UINT8_C(0xA0);
    fixture.image[kArm7RomOffset + 3u] = UINT8_C(0xE1);
    return true;
}

OracisHostProgramSegment arm9_segment(const std::vector<uint8_t> &image)
{
    return {
        kArm9LoadAddress,
        kArm9Size,
        kArm9EntryPoint,
        0u,
        image.data() + kArm9RomOffset
    };
}

OracisHostProgramSegment arm7_segment(const std::vector<uint8_t> &image)
{
    return {
        kArm7LoadAddress,
        kArm7Size,
        kArm7LoadAddress,
        0u,
        image.data() + kArm7RomOffset
    };
}

} // namespace

int main()
{
    Fixture fixture;
    if (!make_fixture(fixture)) {
        return fail("could not create the synthetic encrypted NDS image");
    }
    const std::vector<uint8_t> original_secure_prefix(
        fixture.image.begin() + kArm9RomOffset,
        fixture.image.begin() + kArm9RomOffset +
            oracis::memory::NdsGameCardKey1::SecureAreaEncryptedBytes);

    if (oracis_host_runtime_initialize() != ORACIS_HOST_RUNTIME_OK) {
        return fail("host runtime initialization failed");
    }

    int status = oracis_host_runtime_mount_game_card_image(
        fixture.image.data(), static_cast<uint32_t>(fixture.image.size()));
    if (status != ORACIS_HOST_RUNTIME_OK) {
        (void)oracis_host_runtime_shutdown();
        return fail("could not mount the synthetic Game Card image");
    }

    auto arm9 = arm9_segment(fixture.image);
    auto arm7 = arm7_segment(fixture.image);
    if (oracis_host_runtime_load_game_card_program_segments(&arm9, &arm7) !=
            ORACIS_HOST_RUNTIME_GAME_CARD_ERROR ||
        oracis_host_runtime_program_loaded() != 0 ||
        oracis_host_runtime_global_bus_cycles() != 0u) {
        (void)oracis_host_runtime_shutdown();
        return fail("loader did not require the user-supplied ARM7 BIOS table");
    }

    if (oracis_host_runtime_load_game_card_arm7_bios(
            fixture.arm7_bios.data(),
            static_cast<uint32_t>(fixture.arm7_bios.size())) !=
        ORACIS_HOST_RUNTIME_OK) {
        (void)oracis_host_runtime_shutdown();
        return fail("could not load the synthetic ARM7 BIOS KEY1 table");
    }

    /*
     * Exercise the machine-level IPL publication path with a synthetic,
     * encrypted firmware image. An invalid ARM7 destination must reject the
     * complete pair before the otherwise-valid ARM9 payload is written.
     */
    const uint8_t *key_table = fixture.arm7_bios.data() +
        oracis::memory::NdsGameCardKey1::Arm7BiosKeyTableOffset;
    auto firmware = make_synthetic_firmware(
        key_table, oracis::memory::NdsGameCardKey1::KeyTableBytes);
    if (firmware.size() != 0x290u) {
        (void)oracis_host_runtime_shutdown();
        return fail("could not create the synthetic encrypted firmware IPL");
    }

    auto invalid_destination_firmware = firmware;
    store_u16_le(invalid_destination_firmware, 0x12u, UINT16_C(0xFFFF));
    uint16_t invalid_shift_data = read_u16_le(
        invalid_destination_firmware.data() + 0x14u);
    invalid_shift_data = static_cast<uint16_t>(
        (invalid_shift_data & static_cast<uint16_t>(~UINT16_C(0x0E00))) |
        UINT16_C(0x0E00));
    store_u16_le(invalid_destination_firmware, 0x14u, invalid_shift_data);
    store_u16_le(
        invalid_destination_firmware, 0x2Au,
        crc16_header(invalid_destination_firmware.data() + 0x2Cu, 0x0137u));

    if (oracis_host_runtime_set_spi_firmware_image(
            invalid_destination_firmware.data(),
            static_cast<uint32_t>(invalid_destination_firmware.size())) !=
            ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_load_spi_firmware_boot_code() !=
            ORACIS_HOST_RUNTIME_FIRMWARE_MEMORY_UNAVAILABLE ||
        oracis_host_runtime_last_machine_status() !=
            static_cast<int>(NdsMachineStatus::FirmwareBootMemoryUnavailable) ||
        !oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        bus_read_u32(UINT32_C(0x023FEE00)) != 0u) {
        (void)oracis_host_runtime_shutdown();
        return fail("firmware preflight partially wrote ARM9 before rejecting ARM7");
    }

    if (oracis_host_runtime_set_spi_firmware_image(
            firmware.data(), static_cast<uint32_t>(firmware.size())) !=
            ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_load_spi_firmware_boot_code() !=
            ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_program_loaded() != 0 ||
        oracis_host_runtime_global_bus_cycles() != 0u ||
        !oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        bus_read_u32(UINT32_C(0x023FEE00)) != UINT32_C(0xA4A3A2A1) ||
        !oracis_arm_runtime_select_cpu(NDS_ARM7) ||
        bus_read_u32(UINT32_C(0x0380F700)) != UINT32_C(0xB4B3B2B1) ||
        !oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        (void)oracis_host_runtime_shutdown();
        return fail("firmware IPL parts were not published to ARM9/ARM7 boot RAM");
    }

    auto wrong_metadata = arm9;
    wrong_metadata.load_address += 4u;
    if (oracis_host_runtime_load_game_card_program_segments(
            &wrong_metadata, &arm7) != ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID ||
        oracis_host_runtime_program_loaded() != 0) {
        (void)oracis_host_runtime_shutdown();
        return fail("loader accepted segment metadata that differs from the NDS header");
    }

    std::vector<uint8_t> modified_arm7(
        fixture.image.begin() + kArm7RomOffset, fixture.image.end());
    modified_arm7[0] ^= UINT8_C(0xFF);
    auto wrong_bytes = arm7;
    wrong_bytes.contents = modified_arm7.data();
    if (oracis_host_runtime_load_game_card_program_segments(
            &arm9, &wrong_bytes) != ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID ||
        oracis_host_runtime_program_loaded() != 0) {
        (void)oracis_host_runtime_shutdown();
        return fail("loader accepted segment bytes that differ from the mounted image");
    }

    std::vector<uint8_t> damaged_image = fixture.image;
    damaged_image[kArm9RomOffset] ^= UINT8_C(0x80);
    status = oracis_host_runtime_mount_game_card_image(
        damaged_image.data(), static_cast<uint32_t>(damaged_image.size()));
    if (status != ORACIS_HOST_RUNTIME_OK) {
        (void)oracis_host_runtime_shutdown();
        return fail("could not mount the damaged Secure Area fixture");
    }
    auto damaged_arm9 = arm9_segment(damaged_image);
    auto damaged_arm7 = arm7_segment(damaged_image);
    if (oracis_host_runtime_load_game_card_program_segments(
            &damaged_arm9, &damaged_arm7) != ORACIS_HOST_RUNTIME_GAME_CARD_ERROR ||
        oracis_host_runtime_program_loaded() != 0) {
        (void)oracis_host_runtime_shutdown();
        return fail("loader did not reject an invalid Secure Area identifier");
    }

    status = oracis_host_runtime_mount_game_card_image(
        fixture.image.data(), static_cast<uint32_t>(fixture.image.size()));
    if (status != ORACIS_HOST_RUNTIME_OK) {
        (void)oracis_host_runtime_shutdown();
        return fail("could not remount the valid Secure Area fixture");
    }
    arm9 = arm9_segment(fixture.image);
    arm7 = arm7_segment(fixture.image);

    std::vector<uint8_t> malformed_header = fixture.image;
    write_u32_le(malformed_header, 0x2Cu, UINT32_C(0x7FFFFFFF));
    status = oracis_host_runtime_mount_game_card_image(
        malformed_header.data(),
        static_cast<uint32_t>(malformed_header.size()));
    if (status != ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_load_mounted_game_card_program() !=
            ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID ||
        oracis_host_runtime_program_loaded() != 0) {
        (void)oracis_host_runtime_shutdown();
        return fail("header-driven loader accepted a segment outside the mounted image");
    }

    status = oracis_host_runtime_mount_game_card_image(
        fixture.image.data(), static_cast<uint32_t>(fixture.image.size()));
    if (status != ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_load_mounted_game_card_program() !=
            ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_program_loaded() != 1 ||
        oracis_host_runtime_global_bus_cycles() != 0u) {
        (void)oracis_host_runtime_shutdown();
        return fail("loader failed to load matching ARM9/ARM7 segments after Secure Area decryption");
    }

    constexpr uint32_t kBootHeaderAddress = UINT32_C(0x027FFE00);
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        bus_read_u32(kBootHeaderAddress + UINT32_C(0x0C)) != kGameCode ||
        bus_read_u32(kBootHeaderAddress + UINT32_C(0x20)) != kArm9RomOffset ||
        bus_read_u32(kBootHeaderAddress + UINT32_C(0x34)) != kArm7LoadAddress ||
        bus_read_u32(kArm9LoadAddress) != UINT32_C(0x72636E65) ||
        bus_read_u32(kArm9LoadAddress + 4u) != UINT32_C(0x6A624F79) ||
        bus_read_u32(kArm9LoadAddress + UINT32_C(0x800)) !=
            read_u32_le(fixture.image.data() + kArm9RomOffset + UINT32_C(0x800))) {
        (void)oracis_host_runtime_shutdown();
        return fail("ARM9 RAM did not contain the decrypted Secure Area prefix and unchanged payload");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM7) ||
        bus_read_u32(kArm7LoadAddress) != UINT32_C(0xE1A00000) ||
        !oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        (void)oracis_host_runtime_shutdown();
        return fail("ARM7 segment was not loaded at its header-declared address");
    }

    if (!std::equal(
            original_secure_prefix.begin(), original_secure_prefix.end(),
            fixture.image.begin() + kArm9RomOffset)) {
        (void)oracis_host_runtime_shutdown();
        return fail("loader modified the caller-owned encrypted ROM image");
    }

    if (oracis_host_runtime_shutdown() != ORACIS_HOST_RUNTIME_OK) {
        return fail("host runtime shutdown failed");
    }

    std::cout << "Game Card loader validates metadata, source bytes, Secure Area and boot-header mirror before loading\n";
    return 0;
}
