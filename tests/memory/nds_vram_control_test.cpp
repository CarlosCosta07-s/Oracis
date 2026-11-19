#include "oracis/memory/guest_memory_abi.h"
#include "oracis/memory/nds_vram_control.hpp"
#include "oracis/recomp/runtime_arm.h"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_vram_control_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using oracis::memory::GuestMemory;
    using oracis::memory::NdsVramControl;
    using oracis::memory::Status;

    GuestMemory memory;
    NdsVramControl control(memory);
    if (control.install() != Status::Ok || !control.installed()) {
        return fail("VRAM C/D bank backing and inactive windows did not install");
    }
    if (memory.read(NDS_ARM7, UINT32_C(0x06000000), 4u).status != Status::Unmapped ||
        memory.read(NDS_ARM7, UINT32_C(0x06020000), 4u).status != Status::Unmapped) {
        return fail("VRAM C/D windows were visible before VRAMCNT allocation");
    }

    ArmCpuState arm9{};
    ArmCpuState arm7{};
    arm9.cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7.cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (!oracis_arm_runtime_bind_cpus(&arm9, &arm7) ||
        !oracis_bus_bind_memory(&memory) ||
        !oracis_bus_bind_vram_control(&control)) {
        return fail("CPU and GuestMemory/VRAM bus binding failed");
    }
    if (oracis_bus_bind_vram_control(&control)) {
        return fail("duplicate VRAM controller binding was accepted");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 selection failed");
    }
    bus_write_u8(NdsVramControl::VramCntCAddress, UINT8_C(0xE2));
    if (control.control_c() != UINT8_C(0x82) || control.status() != 1u) {
        return fail("VRAMCNT_C enable/MST/reserved-bit handling failed");
    }
    if (control.read(NDS_ARM9, NdsVramControl::VramCntCAddress, 1u).status !=
            Status::PermissionDenied ||
        control.read(NDS_ARM7, NdsVramControl::VramCntCAddress, 1u).status !=
            Status::CpuAccessDenied ||
        control.write(NDS_ARM7, NdsVramControl::VramCntCAddress, 1u, 0x82u) !=
            Status::CpuAccessDenied) {
        return fail("VRAMCNT_C CPU ownership or write-only behavior failed");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM7)) {
        return fail("ARM7 selection failed");
    }
    if (bus_read_u8(NdsVramControl::VramStatAddress) != 1u) {
        return fail("VRAMSTAT did not report enabled ARM7 bank C");
    }
    bus_write_u32(UINT32_C(0x06000000), UINT32_C(0xC0C0C0C0));
    bus_write_u8(UINT32_C(0x06000004), UINT8_C(0x5A));
    if (bus_read_u32(UINT32_C(0x06000000)) != UINT32_C(0xC0C0C0C0) ||
        bus_read_u8(UINT32_C(0x06000004)) != UINT8_C(0x5A)) {
        return fail("ARM7 did not read/write VRAM C in window zero");
    }
    if (memory.read(NDS_ARM9, UINT32_C(0x06000000), 4u).status !=
        Status::CpuAccessDenied) {
        return fail("ARM9 could access VRAM C while assigned to ARM7");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 reselection failed");
    }
    bus_write_u8(NdsVramControl::VramCntDAddress, UINT8_C(0x8A));
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7)) {
        return fail("ARM7 reselection failed");
    }
    if (bus_read_u8(NdsVramControl::VramStatAddress) != 3u) {
        return fail("VRAMSTAT did not report both ARM7 banks");
    }
    bus_write_u32(UINT32_C(0x06020000), UINT32_C(0xD0D0D0D0));
    if (bus_read_u32(UINT32_C(0x06020000)) != UINT32_C(0xD0D0D0D0) ||
        bus_read_u32(UINT32_C(0x06000000)) != UINT32_C(0xC0C0C0C0)) {
        return fail("VRAM C/D physical banks were aliased to the wrong slots");
    }

    /* Reject same-slot ambiguity transactionally. */
    if (control.write(
            NDS_ARM9, NdsVramControl::VramCntCAddress, 1u, UINT8_C(0x8A)) !=
            Status::Overlap ||
        control.control_c() != UINT8_C(0x82) ||
        control.control_d() != UINT8_C(0x8A) ||
        bus_read_u8(NdsVramControl::VramStatAddress) != 3u ||
        bus_read_u32(UINT32_C(0x06000000)) != UINT32_C(0xC0C0C0C0)) {
        return fail("same-offset VRAM C/D allocation was not rejected atomically");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 reselection before VRAM D disable failed");
    }
    bus_write_u8(NdsVramControl::VramCntDAddress, 0u);
    bus_write_u8(NdsVramControl::VramCntCAddress, UINT8_C(0x8A));
    if (control.status() != 1u ||
        !oracis_arm_runtime_select_cpu(NDS_ARM7)) {
        return fail("VRAM C remap to offset one failed");
    }
    if (bus_read_u8(NdsVramControl::VramStatAddress) != 1u ||
        memory.read(NDS_ARM7, UINT32_C(0x06000000), 4u).status != Status::Unmapped ||
        bus_read_u32(UINT32_C(0x06020000)) != UINT32_C(0xC0C0C0C0)) {
        return fail("VRAM C data did not follow the remapped physical bank");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 reselection before invalid-OFS check failed");
    }
    bus_write_u8(NdsVramControl::VramCntCAddress, UINT8_C(0x92));
    if (control.status() != 1u ||
        memory.read(NDS_ARM7, UINT32_C(0x06000000), 4u).status != Status::Unmapped ||
        memory.read(NDS_ARM7, UINT32_C(0x06020000), 4u).status != Status::Unmapped) {
        return fail("VRAMSTAT incorrectly reflected OFS or invalid offset was mapped");
    }

    bus_write_u8(NdsVramControl::VramCntCAddress, 0u);
    if (control.status() != 0u ||
        memory.read(NDS_ARM7, UINT32_C(0x06000000), 4u).status != Status::Unmapped) {
        return fail("disabling VRAM C did not remove its ARM7 visibility");
    }

    if (control.read(NDS_ARM7, NdsVramControl::VramStatAddress, 2u).status !=
            Status::InvalidWidth ||
        control.write(NDS_ARM7, NdsVramControl::VramStatAddress, 1u, 1u) !=
            Status::PermissionDenied ||
        control.read(NDS_ARM9, NdsVramControl::VramStatAddress, 1u).status !=
            Status::Unmapped) {
        return fail("VRAMSTAT access width, write protection or ARM9 alias failed");
    }

    /* MST=0 exposes C/D to ARM9 at their fixed LCDC physical addresses. */
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 reselection before LCDC mapping failed");
    }
    bus_write_u8(NdsVramControl::VramCntCAddress, UINT8_C(0x98));
    bus_write_u8(NdsVramControl::VramCntDAddress, UINT8_C(0x80));
    if (control.status() != 0u ||
        memory.read(NDS_ARM9, UINT32_C(0x06840000), 4u).status != Status::Ok ||
        memory.read(NDS_ARM9, UINT32_C(0x06860000), 4u).status != Status::Ok ||
        memory.read(NDS_ARM7, UINT32_C(0x06840000), 4u).status == Status::Ok ||
        memory.read(NDS_ARM7, UINT32_C(0x06860000), 4u).status == Status::Ok) {
        return fail("MST=0 did not expose LCDC C/D banks exclusively to ARM9");
    }
    bus_write_u32(UINT32_C(0x06840000), UINT32_C(0xCCCCCCCC));
    bus_write_u32(UINT32_C(0x06860000), UINT32_C(0xDDDDDDDD));
    if (bus_read_u32(UINT32_C(0x06840000)) != UINT32_C(0xCCCCCCCC) ||
        bus_read_u32(UINT32_C(0x06860000)) != UINT32_C(0xDDDDDDDD)) {
        return fail("ARM9 LCDC C/D aliases did not preserve separate bank data");
    }

    /* Moving C from LCDC to ARM7 Work RAM keeps the physical bank contents. */
    bus_write_u8(NdsVramControl::VramCntCAddress, UINT8_C(0x82));
    if (control.status() != 1u ||
        !oracis_arm_runtime_select_cpu(NDS_ARM7) ||
        bus_read_u8(NdsVramControl::VramStatAddress) != 1u ||
        bus_read_u32(UINT32_C(0x06000000)) != UINT32_C(0xCCCCCCCC) ||
        memory.read(NDS_ARM9, UINT32_C(0x06840000), 4u).status == Status::Ok) {
        return fail("C bank data or CPU permissions were lost when changing LCDC to ARM7 mode");
    }

    /* All nine banks expose their fixed LCDC aliases in MST=0. */
    struct LcdcBankCase {
        uint32_t control_address;
        uint32_t guest_address;
        uint32_t size;
        uint32_t control_value;
        uint32_t marker;
    };
    constexpr LcdcBankCase lcdc_banks[] = {
        {NdsVramControl::VramCntAAddress, UINT32_C(0x06800000),
         UINT32_C(0x00020000), UINT32_C(0x98), UINT32_C(0xA0A0A0A0)},
        {NdsVramControl::VramCntBAddress, UINT32_C(0x06820000),
         UINT32_C(0x00020000), UINT32_C(0x90), UINT32_C(0xB1B1B1B1)},
        {NdsVramControl::VramCntCAddress, UINT32_C(0x06840000),
         UINT32_C(0x00020000), UINT32_C(0x98), UINT32_C(0xC2C2C2C2)},
        {NdsVramControl::VramCntDAddress, UINT32_C(0x06860000),
         UINT32_C(0x00020000), UINT32_C(0x90), UINT32_C(0xD3D3D3D3)},
        {NdsVramControl::VramCntEAddress, UINT32_C(0x06880000),
         UINT32_C(0x00010000), UINT32_C(0x98), UINT32_C(0xE4E4E4E4)},
        {NdsVramControl::VramCntFAddress, UINT32_C(0x06890000),
         UINT32_C(0x00004000), UINT32_C(0x98), UINT32_C(0xF5F5F5F5)},
        {NdsVramControl::VramCntGAddress, UINT32_C(0x06894000),
         UINT32_C(0x00004000), UINT32_C(0x90), UINT32_C(0x16161616)},
        {NdsVramControl::VramCntHAddress, UINT32_C(0x06898000),
         UINT32_C(0x00008000), UINT32_C(0x98), UINT32_C(0x27272727)},
        {NdsVramControl::VramCntIAddress, UINT32_C(0x068A0000),
         UINT32_C(0x00004000), UINT32_C(0x90), UINT32_C(0x38383838)}
    };
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 selection before all-bank LCDC test failed");
    }
    for (const LcdcBankCase &bank : lcdc_banks) {
        bus_write_u8(bank.control_address, bank.control_value);
    }
    for (const LcdcBankCase &bank : lcdc_banks) {
        bus_write_u32(bank.guest_address, bank.marker);
        bus_write_u32(
            bank.guest_address + bank.size - 4u, ~bank.marker);
    }
    for (const LcdcBankCase &bank : lcdc_banks) {
        if (bus_read_u32(bank.guest_address) != bank.marker ||
            bus_read_u32(bank.guest_address + bank.size - 4u) != ~bank.marker ||
            memory.read(
                NDS_ARM7, bank.guest_address, 4u).status == Status::Ok) {
            return fail("LCDC MST=0 bank base, size or CPU visibility is incorrect");
        }
    }
    if (control.status() != 0u ||
        !oracis_arm_runtime_select_cpu(NDS_ARM7) ||
        bus_read_u8(NdsVramControl::VramStatAddress) != 0u) {
        return fail("VRAMSTAT did not clear when C/D left ARM7 Work RAM mode");
    }

    if (oracis_bus_unbind_memory(&memory)) {
        return fail("memory unbind succeeded while VRAM controller remained bound");
    }
    if (!oracis_bus_unbind_vram_control(&control) ||
        !oracis_bus_unbind_memory(&memory) ||
        !oracis_arm_runtime_unbind_cpus(&arm9, &arm7)) {
        return fail("VRAM/GuestMemory/CPU unbinding failed");
    }

    std::puts("VRAM A-I LCDC mapping, C/D ARM7 mapping, VRAMSTAT and remapping passed");
    return 0;
}
