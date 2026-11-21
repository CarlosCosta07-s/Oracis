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
    if (oracis_host_runtime_load_game_card_program_segments(&arm9, &arm7) !=
            ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_program_loaded() != 1 ||
        oracis_host_runtime_global_bus_cycles() != 0u) {
        (void)oracis_host_runtime_shutdown();
        return fail("loader failed to load matching ARM9/ARM7 segments after Secure Area decryption");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9) ||
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

    std::cout << "Game Card loader validates metadata, source bytes and Secure Area before loading\n";
    return 0;
}
