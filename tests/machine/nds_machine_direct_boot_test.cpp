#include "oracis/machine/nds_machine.hpp"

#include "oracis/memory/guest_memory_abi.h"
#include "oracis/memory/nds_vram_control.hpp"
#include "oracis/memory/nds_wram_control.hpp"
#include "oracis/recomp/runtime_arm.h"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_machine_direct_boot_test: %s\\n", message);
    return 1;
}

} // namespace

int main()
{
    using oracis::machine::NdsMachine;
    using oracis::machine::NdsMachineStatus;
    using oracis::machine::NdsProgramSegment;
    using oracis::memory::NdsBiosControl;
    using oracis::memory::NdsVramControl;
    using oracis::memory::NdsWramControl;
    using oracis::memory::SharedWramMode;
    using oracis::memory::Status;

    NdsMachine machine(SharedWramMode::Arm9All32K);
    if (machine.initialize() != NdsMachineStatus::Ok) {
        return fail("machine initialization failed");
    }

    std::array<uint8_t, NdsBiosControl::Arm7BiosBytes> arm7_bios{};
    std::array<uint8_t, NdsBiosControl::Arm9BiosBytes> arm9_bios{};
    for (uint32_t i = 0u; i < arm7_bios.size(); ++i) {
        arm7_bios[i] = static_cast<uint8_t>((i * 11u + 5u) & 0xFFu);
    }
    for (uint32_t i = 0u; i < arm9_bios.size(); ++i) {
        arm9_bios[i] = static_cast<uint8_t>((i * 13u + 9u) & 0xFFu);
    }
    if (machine.bios_control().load_arm7_bios(
            arm7_bios.data(), static_cast<uint32_t>(arm7_bios.size())) != Status::Ok ||
        machine.bios_control().load_arm9_bios(
            arm9_bios.data(), static_cast<uint32_t>(arm9_bios.size())) != Status::Ok) {
        return fail("machine rejected synthetic user BIOS images");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        bus_read_u32(NdsBiosControl::Arm9BiosBase) !=
            (static_cast<uint32_t>(arm9_bios[0]) |
             (static_cast<uint32_t>(arm9_bios[1]) << 8u) |
             (static_cast<uint32_t>(arm9_bios[2]) << 16u) |
             (static_cast<uint32_t>(arm9_bios[3]) << 24u))) {
        return fail("ARM9 BIOS ROM was not visible through the ARM9 bus");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM7)) {
        return fail("could not select ARM7 to validate its BIOS window");
    }
    ArmCpuState *arm7_state = machine.cpu_state(NDS_ARM7);
    if (arm7_state == nullptr) return fail("ARM7 state was unavailable");

    arm7_state->R[15] = UINT32_C(0x00002000);
    const uint32_t bios_first_word =
        static_cast<uint32_t>(arm7_bios[0]) |
        (static_cast<uint32_t>(arm7_bios[1]) << 8u) |
        (static_cast<uint32_t>(arm7_bios[2]) << 16u) |
        (static_cast<uint32_t>(arm7_bios[3]) << 24u);
    if (bus_read_u32(NdsBiosControl::Arm7BiosBase) != bios_first_word) {
        return fail("ARM7 BIOS was not readable while PC remained in BIOS");
    }

    bus_write_u32(
        NdsBiosControl::BiosProtectionAddress, UINT32_C(0x00001205));
    if (bus_read_u32(NdsBiosControl::BiosProtectionAddress) !=
        UINT32_C(0x00001204)) {
        return fail("ARM7 BIOSPROT register did not read back 0x1204");
    }

    arm7_state->R[15] = UINT32_C(0x00001300);
    const uint32_t bios_high_word =
        static_cast<uint32_t>(arm7_bios[0x1204u]) |
        (static_cast<uint32_t>(arm7_bios[0x1205u]) << 8u) |
        (static_cast<uint32_t>(arm7_bios[0x1206u]) << 16u) |
        (static_cast<uint32_t>(arm7_bios[0x1207u]) << 24u);
    if (bus_read_u32(NdsBiosControl::Arm7BiosBase) != UINT32_MAX ||
        bus_read_u32(UINT32_C(0x00001204)) != bios_high_word) {
        return fail("ARM7 BIOS bus did not enforce the locked lower region");
    }
    arm7_state->R[15] = UINT32_C(0x00004000);
    if (bus_read_u32(UINT32_C(0x00001204)) != UINT32_MAX) {
        return fail("ARM7 BIOS bus exposed protected bytes after PC left BIOS");
    }

    constexpr uint8_t arm9_code[] = {
        UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0xA0), UINT8_C(0xE1)
    };
    constexpr uint8_t arm7_code[] = {
        UINT8_C(0x70), UINT8_C(0x47)
    };
    const NdsProgramSegment arm9_image = {
        NDS_ARM9, UINT32_C(0x02006000),
        static_cast<uint32_t>(sizeof(arm9_code)),
        UINT32_C(0x02006000), false, arm9_code
    };
    const NdsProgramSegment arm7_image = {
        NDS_ARM7, UINT32_C(0x02381000),
        static_cast<uint32_t>(sizeof(arm7_code)),
        UINT32_C(0x02381000), true, arm7_code
    };

    if (machine.load_program_segments(arm9_image, arm7_image) !=
            NdsMachineStatus::Ok ||
        !machine.program_loaded()) {
        return fail("direct-entry program load or memory-profile setup failed");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM7) ||
        bus_read_u8(NdsWramControl::WramStatAddress) != UINT8_C(3) ||
        bus_read_u8(NdsVramControl::VramStatAddress) != UINT8_C(3)) {
        return fail("WRAMCNT=3 or ARM7 VRAM C/D allocation was not seeded");
    }

    /* 0x037F8000 must be Shared WRAM, not an alias of private WRAM. */
    bus_write_u32(UINT32_C(0x037F8000), UINT32_C(0x12345678));
    bus_write_u32(UINT32_C(0x03808000), UINT32_C(0xAABBCCDD));
    if (bus_read_u32(UINT32_C(0x037F8000)) != UINT32_C(0x12345678) ||
        bus_read_u32(UINT32_C(0x03808000)) != UINT32_C(0xAABBCCDD)) {
        return fail("ARM7 Shared WRAM and private WRAM aliases overlapped");
    }

    bus_write_u32(UINT32_C(0x06000000), UINT32_C(0xC0C0C0C0));
    bus_write_u32(UINT32_C(0x06020000), UINT32_C(0xD0D0D0D0));
    if (bus_read_u32(UINT32_C(0x06000000)) != UINT32_C(0xC0C0C0C0) ||
        bus_read_u32(UINT32_C(0x06020000)) != UINT32_C(0xD0D0D0D0)) {
        return fail("ARM7 VRAM C/D work-RAM windows were not independent");
    }

    const auto arm9_shared_wram =
        machine.guest_memory().read(NDS_ARM9, UINT32_C(0x03000000), 4u);
    const auto arm9_vram_c =
        machine.guest_memory().read(NDS_ARM9, UINT32_C(0x06000000), 4u);
    const auto arm9_vram_d =
        machine.guest_memory().read(NDS_ARM9, UINT32_C(0x06020000), 4u);
    if (arm9_shared_wram.status == Status::Ok ||
        arm9_vram_c.status == Status::Ok ||
        arm9_vram_d.status == Status::Ok) {
        return fail("ARM9 retained access to memory assigned to ARM7");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        bus_read_u8(NdsWramControl::WramCntAddress) != UINT8_C(3)) {
        return fail("ARM9 WRAMCNT readback did not preserve mode 3");
    }

    if (machine.shutdown() != NdsMachineStatus::Ok) {
        return fail("machine shutdown failed");
    }

    return 0;
}
