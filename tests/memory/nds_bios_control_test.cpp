#include "oracis/memory/nds_bios_control.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

using oracis::memory::GuestMemory;
using oracis::memory::NdsBiosControl;
using oracis::memory::Status;

int fail(const char *message)
{
    std::fprintf(stderr, "nds_bios_control_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    GuestMemory memory;
    NdsBiosControl bios(memory);
    std::array<uint8_t, NdsBiosControl::Arm7BiosBytes> arm7{};
    std::array<uint8_t, NdsBiosControl::Arm9BiosBytes> arm9{};

    for (uint32_t i = 0u; i < arm7.size(); ++i) {
        arm7[i] = static_cast<uint8_t>((i * 17u + 3u) & 0xFFu);
    }
    for (uint32_t i = 0u; i < arm9.size(); ++i) {
        arm9[i] = static_cast<uint8_t>((i * 29u + 7u) & 0xFFu);
    }

    if (bios.load_arm7_bios(nullptr, NdsBiosControl::Arm7BiosBytes) !=
            Status::InvalidRegion ||
        bios.load_arm7_bios(arm7.data(), NdsBiosControl::Arm7BiosBytes - 1u) !=
            Status::InvalidRegion ||
        bios.load_arm9_bios(arm9.data(), NdsBiosControl::Arm9BiosBytes - 1u) !=
            Status::InvalidRegion) {
        return fail("malformed user BIOS images were accepted");
    }

    if (bios.load_arm7_bios(arm7.data(), static_cast<uint32_t>(arm7.size())) !=
            Status::Ok ||
        bios.load_arm9_bios(arm9.data(), static_cast<uint32_t>(arm9.size())) !=
            Status::Ok ||
        !bios.arm7_bios_loaded() || !bios.arm9_bios_loaded()) {
        return fail("valid synthetic CPU-local BIOS images were not mapped");
    }

    const uint32_t expected_arm7 =
        static_cast<uint32_t>(arm7[0]) |
        (static_cast<uint32_t>(arm7[1]) << 8u) |
        (static_cast<uint32_t>(arm7[2]) << 16u) |
        (static_cast<uint32_t>(arm7[3]) << 24u);
    const uint32_t expected_arm9 =
        static_cast<uint32_t>(arm9[0]) |
        (static_cast<uint32_t>(arm9[1]) << 8u) |
        (static_cast<uint32_t>(arm9[2]) << 16u) |
        (static_cast<uint32_t>(arm9[3]) << 24u);
    if (memory.read(NDS_ARM7, NdsBiosControl::Arm7BiosBase, 4u).value != expected_arm7 ||
        memory.read(NDS_ARM9, NdsBiosControl::Arm9BiosBase, 4u).value != expected_arm9) {
        return fail("BIOS contents were not copied into their mapped windows");
    }

    if (bios.read(NDS_ARM7, NdsBiosControl::Arm9BiosBase, 4u, 0u).status !=
            Status::Unmapped ||
        bios.read(NDS_ARM9, NdsBiosControl::Arm7BiosBase, 4u, 0u).status !=
            Status::Unmapped ||
        memory.write(NDS_ARM7, NdsBiosControl::Arm7BiosBase, 1u, 0xAAu) !=
            Status::PermissionDenied ||
        memory.write(NDS_ARM9, NdsBiosControl::Arm9BiosBase, 1u, 0xAAu) !=
            Status::PermissionDenied) {
        return fail("BIOS mappings were writable or visible to the wrong CPU");
    }

    const auto blocked_after_boot = bios.read(
        NDS_ARM7, 0u, 4u, UINT32_C(0x00004000));
    const auto visible_inside_bios = bios.read(
        NDS_ARM7, 0u, 4u, UINT32_C(0x00002000));
    if (!blocked_after_boot ||
        blocked_after_boot.value != UINT32_MAX ||
        !visible_inside_bios ||
        visible_inside_bios.value != expected_arm7) {
        return fail("default ARM7 BIOS PC protection behavior was incorrect");
    }

    if (bios.write(
            NDS_ARM7, NdsBiosControl::BiosProtectionAddress, 4u,
            UINT32_C(0x00001205)) != Status::Ok ||
        bios.protection_boundary() != UINT32_C(0x00001204) ||
        !bios.protection_locked() ||
        bios.read(NDS_ARM7, NdsBiosControl::BiosProtectionAddress, 4u, 0u).value !=
            UINT32_C(0x00001204)) {
        return fail("BIOSPROT did not ignore bit 0 or retain the aligned boundary");
    }

    const auto protected_low = bios.read(
        NDS_ARM7, UINT32_C(0x00000100), 4u, UINT32_C(0x00001300));
    const auto readable_high = bios.read(
        NDS_ARM7, UINT32_C(0x00001204), 4u, UINT32_C(0x00001300));
    const auto readable_all_inside_low = bios.read(
        NDS_ARM7, UINT32_C(0x00000100), 4u, UINT32_C(0x00000100));
    const auto blocked_outside_bios = bios.read(
        NDS_ARM7, UINT32_C(0x00001204), 4u, UINT32_C(0x00004000));
    if (!protected_low || protected_low.value != UINT32_MAX ||
        !readable_high ||
        readable_high.value != memory.read(
            NDS_ARM7, UINT32_C(0x00001204), 4u).value ||
        !readable_all_inside_low ||
        readable_all_inside_low.value != memory.read(
            NDS_ARM7, UINT32_C(0x00000100), 4u).value ||
        !blocked_outside_bios || blocked_outside_bios.value != UINT32_MAX) {
        return fail("ARM7 BIOSPROT split-region PC checks were incorrect");
    }

    if (bios.write(
            NDS_ARM7, NdsBiosControl::BiosProtectionAddress, 4u, 0u) !=
            Status::Ok ||
        bios.protection_boundary() != UINT32_C(0x00001204) ||
        bios.write(
            NDS_ARM9, NdsBiosControl::BiosProtectionAddress, 4u, 0u) !=
            Status::Unmapped ||
        bios.read(NDS_ARM7, 0u, 3u, 0u).status != Status::InvalidWidth ||
        bios.read(NDS_ARM7, UINT32_C(0x00003FFF), 4u, 0u).status !=
            Status::CrossesRegion) {
        return fail("BIOSPROT write-once, CPU or access-width checks failed");
    }

    if (bios.unload_arm9_bios() != Status::Ok ||
        bios.unload_arm7_bios() != Status::Ok ||
        bios.arm9_bios_loaded() || bios.arm7_bios_loaded() ||
        memory.read(NDS_ARM9, NdsBiosControl::Arm9BiosBase, 4u).status !=
            Status::Unmapped ||
        memory.read(NDS_ARM7, NdsBiosControl::Arm7BiosBase, 4u).status !=
            Status::Unmapped) {
        return fail("BIOS ROM mappings were not removed cleanly");
    }

    std::puts("NDS CPU-local BIOS mapping and ARM7 BIOSPROT tests passed");
    return 0;
}
