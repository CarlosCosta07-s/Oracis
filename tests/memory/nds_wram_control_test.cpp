#include "oracis/memory/guest_memory_abi.h"
#include "oracis/memory/nds_wram_control.hpp"
#include "oracis/recomp/runtime_arm.h"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_wram_control_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using oracis::memory::GuestMemory;
    using oracis::memory::NdsWramControl;
    using oracis::memory::NdsRetailRamMap;
    using oracis::memory::SharedWramMode;
    using oracis::memory::Status;

    GuestMemory memory;
    NdsWramControl control(memory, SharedWramMode::Arm9All32K);
    const Status install_status = control.install();
    if (install_status != Status::Ok) {
        std::fprintf(stderr, "initial install status: %s\\n",
                     GuestMemory::status_name(install_status));
        return fail("initial retail RAM install failed");
    }

    ArmCpuState arm9{};
    ArmCpuState arm7{};
    arm9.cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7.cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (!oracis_arm_runtime_bind_cpus(&arm9, &arm7))
        return fail("CPU state bind failed");
    if (!oracis_bus_bind_memory(&memory))
        return fail("GuestMemory bus bind failed");
    if (!oracis_bus_bind_wram_control(&control))
        return fail("WRAM MMIO bind failed");
    if (oracis_bus_bind_wram_control(&control))
        return fail("duplicate MMIO bind should be rejected");

    if (bus_read_u8(NdsWramControl::WramCntAddress) != 0u)
        return fail("WRAMCNT initial mode readback was not zero");

    constexpr uint32_t bank0 = UINT32_C(0xB0B0B0B0);
    constexpr uint32_t bank1 = UINT32_C(0xB1B1B1B1);
    constexpr uint32_t private_value = UINT32_C(0xA7A7A7A7);

    bus_write_u32(0x03000000u, bank0);
    bus_write_u32(0x03004000u, bank1);
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7))
        return fail("ARM7 initial private-WRAM selection failed");
    bus_write_u32(NdsRetailRamMap::Arm7WramBase, private_value);
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9))
        return fail("ARM9 selection after private-WRAM seed failed");

    /* Mode 1: ARM9 bank 1, ARM7 bank 0, same guest base per CPU. */
    bus_write_u8(NdsWramControl::WramCntAddress, 1u);
    if (control.mode() != SharedWramMode::Arm9Bank1Arm7Bank0 ||
        bus_read_u8(NdsWramControl::WramCntAddress) != 1u ||
        bus_read_u32(0x03000000u) != bank1) {
        return fail("ARM9 WRAMCNT mode 1 update failed");
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7))
        return fail("ARM7 select failed");
    if (bus_read_u32(0x03000000u) != bank0)
        return fail("ARM7 did not receive physical bank 0 in mode 1");

    /* ARM7 is not allowed to write ARM9's WRAMCNT. */
    if (control.write(NDS_ARM7, NdsWramControl::WramCntAddress,
                      1u, 2u) != Status::CpuAccessDenied) {
        return fail("ARM7 WRAMCNT write was not rejected");
    }
    if (control.read(NDS_ARM7, NdsWramControl::WramCntAddress, 1u).status !=
        Status::CpuAccessDenied) {
        return fail("ARM7 WRAMCNT read was not rejected");
    }

    /* Mode 2 reverses the assignment; upper reserved bits are ignored. */
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9))
        return fail("ARM9 select failed");
    bus_write_u8(NdsWramControl::WramCntAddress, 0xFEu);
    if (control.mode() != SharedWramMode::Arm9Bank0Arm7Bank1 ||
        bus_read_u8(NdsWramControl::WramCntAddress) != 2u ||
        bus_read_u32(0x03000000u) != bank0) {
        return fail("WRAMCNT mode 2 or reserved-bit masking failed");
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7))
        return fail("ARM7 reselection failed");
    if (bus_read_u32(0x03000000u) != bank1)
        return fail("ARM7 did not receive physical bank 1 in mode 2");

    /* Mode 3 gives both physical banks to ARM7, contiguous and mirrored. */
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9))
        return fail("ARM9 selection before mode 3 write failed");
    bus_write_u8(NdsWramControl::WramCntAddress, 3u);
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7))
        return fail("ARM7 selection for mode 3 verification failed");
    if (bus_read_u32(0x03000000u) != bank0 ||
        bus_read_u32(0x03004000u) != bank1) {
        return fail("WRAMCNT mode 3 did not expose both ARM7 banks");
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9))
        return fail("ARM9 reselection failed");
    if (control.read(NDS_ARM9, NdsWramControl::WramStatAddress, 1u).status !=
        Status::Unmapped) {
        return fail("ARM9 VRAMCNT_B address was incorrectly handled as WRAMSTAT");
    }

    /* Return to mode 0; ARM7 window must mirror private WRAM. */
    bus_write_u8(NdsWramControl::WramCntAddress, 0u);
    if (bus_read_u32(0x03000000u) != bank0 ||
        bus_read_u32(0x03004000u) != bank1) {
        return fail("WRAMCNT mode 0 did not restore ARM9 Shared WRAM");
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7))
        return fail("ARM7 final selection failed");
    if (bus_read_u32(0x03000000u) != private_value ||
        bus_read_u32(NdsRetailRamMap::Arm7WramBase) != private_value) {
        return fail("WRAMCNT mode 0 did not mirror ARM7 private WRAM");
    }

    if (oracis_bus_unbind_memory(&memory))
        return fail("memory unbind must be rejected while the device is bound");
    if (!oracis_bus_unbind_wram_control(&control))
        return fail("WRAM control unbind failed");
    if (!oracis_bus_unbind_memory(&memory))
        return fail("GuestMemory unbind failed");

    std::puts("WRAMCNT MMIO CPU permissions, bus routing and remapping passed");
    return 0;
}
