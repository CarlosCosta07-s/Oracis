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
            Status::PermissionDenied) {
        return fail("VRAMSTAT/VRAMCNT_A access width, write protection or CPU ownership failed");
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
         UINT32_C(0x00020000), UINT32_C(0x9C), UINT32_C(0xA0A0A0A0)},
        {NdsVramControl::VramCntBAddress, UINT32_C(0x06820000),
         UINT32_C(0x00020000), UINT32_C(0x94), UINT32_C(0xB1B1B1B1)},
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
         UINT32_C(0x00008000), UINT32_C(0x9C), UINT32_C(0x27272727)},
        {NdsVramControl::VramCntIAddress, UINT32_C(0x068A0000),
         UINT32_C(0x00004000), UINT32_C(0x94), UINT32_C(0x38383838)}
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
        /* LCDC VRAM ignores STRB; ARM7 C/D Work RAM byte stores are tested above. */
        bus_write_u8(
            bank.guest_address,
            static_cast<uint8_t>((bank.marker & UINT32_C(0xFF)) ^ UINT32_C(0xFF)));
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

    /* Main Engine A BG slots: VRAM A-D occupy the four 128KB offsets. */
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 selection before main BG/OBJ mapping failed");
    }
    constexpr uint32_t bg_registers[] = {
        NdsVramControl::VramCntAAddress,
        NdsVramControl::VramCntBAddress,
        NdsVramControl::VramCntCAddress,
        NdsVramControl::VramCntDAddress
    };
    constexpr uint32_t bg_addresses[] = {
        UINT32_C(0x06000000), UINT32_C(0x06020000),
        UINT32_C(0x06040000), UINT32_C(0x06060000)
    };
    constexpr uint32_t bg_markers[] = {
        UINT32_C(0xA1A1A1A1), UINT32_C(0xB2B2B2B2),
        UINT32_C(0xC3C3C3C3), UINT32_C(0xD4D4D4D4)
    };
    constexpr uint8_t bg_controls[] = {
        UINT8_C(0x85), UINT8_C(0x8D), UINT8_C(0x91), UINT8_C(0x99)
    };
    for (uint32_t i = 0u; i < 4u; ++i) {
        bus_write_u8(bg_registers[i], bg_controls[i]);
    }
    for (uint32_t i = 0u; i < 4u; ++i) {
        bus_write_u32(bg_addresses[i], bg_markers[i]);
        bus_write_u32(bg_addresses[i] + NdsVramControl::BankSize - 4u,
                      ~bg_markers[i]);
    }
    for (uint32_t i = 0u; i < 4u; ++i) {
        if (bus_read_u32(bg_addresses[i]) != bg_markers[i] ||
            bus_read_u32(bg_addresses[i] + NdsVramControl::BankSize - 4u) !=
                ~bg_markers[i] ||
            memory.read(NDS_ARM7, bg_addresses[i], 4u).status == Status::Ok) {
            return fail("main Engine A BG bank/offset mapping failed");
        }
    }

    /* Duplicate slot assignment is ambiguous and must preserve prior views. */
    if (control.write(
            NDS_ARM9, NdsVramControl::VramCntBAddress, 1u, UINT8_C(0x81)) !=
            Status::Overlap ||
        bus_read_u32(bg_addresses[0]) != bg_markers[0] ||
        bus_read_u32(bg_addresses[1]) != bg_markers[1]) {
        return fail("duplicate Main BG slot was not rejected transactionally");
    }

    /* Main Engine A OBJ supports A/B only, with OFS selecting one of two slots. */
    bus_write_u8(NdsVramControl::VramCntAAddress, UINT8_C(0x86));
    bus_write_u8(NdsVramControl::VramCntBAddress, UINT8_C(0x8E));
    bus_write_u32(UINT32_C(0x06400000), UINT32_C(0xAA55AA55));
    bus_write_u32(UINT32_C(0x06420000), UINT32_C(0xBB66BB66));
    bus_write_u8(UINT32_C(0x06400000), UINT8_C(0x00));
    bus_write_u8(UINT32_C(0x06420000), UINT8_C(0x00));
    if (bus_read_u32(UINT32_C(0x06400000)) != UINT32_C(0xAA55AA55) ||
        bus_read_u32(UINT32_C(0x06420000)) != UINT32_C(0xBB66BB66)) {
        return fail("main Engine A OBJ mapping or ignored byte stores failed");
    }

    /* Engine B BG/OBJ are fixed C/D destinations at MST=4. */
    bus_write_u8(NdsVramControl::VramCntCAddress, UINT8_C(0x9C));
    bus_write_u8(NdsVramControl::VramCntDAddress, UINT8_C(0x94));
    bus_write_u32(UINT32_C(0x06200000), UINT32_C(0xCC77CC77));
    bus_write_u32(UINT32_C(0x06600000), UINT32_C(0xDD88DD88));
    if (bus_read_u32(UINT32_C(0x06200000)) != UINT32_C(0xCC77CC77) ||
        bus_read_u32(UINT32_C(0x06600000)) != UINT32_C(0xDD88DD88)) {
        return fail("Engine B BG/OBJ C/D destinations failed");
    }


    /*
     * E/F/G complete the CPU-visible Main Engine A BG/OBJ subset.
     * F/G select one of four 16KB placements:
     * base + 0x4000*OFS.0 + 0x10000*OFS.1.
     */
    constexpr uint32_t all_vram_controls[] = {
        NdsVramControl::VramCntAAddress,
        NdsVramControl::VramCntBAddress,
        NdsVramControl::VramCntCAddress,
        NdsVramControl::VramCntDAddress,
        NdsVramControl::VramCntEAddress,
        NdsVramControl::VramCntFAddress,
        NdsVramControl::VramCntGAddress,
        NdsVramControl::VramCntHAddress,
        NdsVramControl::VramCntIAddress
    };
    for (uint32_t register_address : all_vram_controls) {
        bus_write_u8(register_address, 0u);
    }

    const auto compact_bank_address = [](uint32_t base, uint32_t ofs) {
        return base + ((ofs & 1u) * UINT32_C(0x00004000)) +
            ((ofs & 2u) * UINT32_C(0x00008000));
    };
    const auto exercise_compact_bank = [&](uint32_t register_address,
                                           uint32_t mst,
                                           uint32_t base_address) {
        for (uint32_t ofs = 0u; ofs < 4u; ++ofs) {
            const uint8_t setting = static_cast<uint8_t>(
                UINT32_C(0x80) | mst | (ofs << 3u));
            bus_write_u8(register_address, setting);
            const uint32_t target = compact_bank_address(base_address, ofs);
            const uint32_t marker = UINT32_C(0xF0000000) |
                (mst << 24u) | (ofs << 16u) |
                (register_address & UINT32_C(0x0000FFFF));
            bus_write_u32(target, marker);
            /* STRB is ignored in all ARM9 VRAM CPU windows. */
            bus_write_u8(target, UINT8_C(0x00));
            if (memory.read(NDS_ARM9, target, 4u).status != Status::Ok ||
                bus_read_u32(target) != marker) {
                return false;
            }
            for (uint32_t other = 0u; other < 4u; ++other) {
                if (other == ofs) continue;
                const uint32_t inactive = compact_bank_address(base_address, other);
                if (memory.read(NDS_ARM9, inactive, 4u).status == Status::Ok) {
                    return false;
                }
            }
        }
        bus_write_u8(register_address, 0u);
        return true;
    };

    if (!exercise_compact_bank(
            NdsVramControl::VramCntFAddress, 1u, UINT32_C(0x06000000)) ||
        !exercise_compact_bank(
            NdsVramControl::VramCntGAddress, 1u, UINT32_C(0x06000000)) ||
        !exercise_compact_bank(
            NdsVramControl::VramCntFAddress, 2u, UINT32_C(0x06400000)) ||
        !exercise_compact_bank(
            NdsVramControl::VramCntGAddress, 2u, UINT32_C(0x06400000))) {
        return fail("VRAM F/G Main BG/OBJ OFS bit placement or byte-store behavior failed");
    }

    /* Bank E ignores OFS for both Main BG and Main OBJ. */
    bus_write_u8(NdsVramControl::VramCntEAddress, UINT8_C(0x99));
    bus_write_u32(UINT32_C(0x06000000), UINT32_C(0xE1E1E1E1));
    bus_write_u32(UINT32_C(0x0600FFFC), UINT32_C(0xE2E2E2E2));
    if (bus_read_u32(UINT32_C(0x06000000)) != UINT32_C(0xE1E1E1E1) ||
        bus_read_u32(UINT32_C(0x0600FFFC)) != UINT32_C(0xE2E2E2E2) ||
        memory.read(NDS_ARM7, UINT32_C(0x06000000), 4u).status == Status::Ok) {
        return fail("VRAM E Main BG 64KB window or ignored OFS failed");
    }
    bus_write_u8(NdsVramControl::VramCntEAddress, UINT8_C(0x9A));
    bus_write_u32(UINT32_C(0x06400000), UINT32_C(0xE3E3E3E3));
    bus_write_u32(UINT32_C(0x0640FFFC), UINT32_C(0xE4E4E4E4));
    if (bus_read_u32(UINT32_C(0x06400000)) != UINT32_C(0xE3E3E3E3) ||
        bus_read_u32(UINT32_C(0x0640FFFC)) != UINT32_C(0xE4E4E4E4)) {
        return fail("VRAM E Main OBJ 64KB window or ignored OFS failed");
    }

    /*
     * Engine B supports C/D in MST=4, H/I in MST=1 for BG, and I in MST=2
     * for OBJ. OFS is ignored for H and I.
     */
    bus_write_u8(NdsVramControl::VramCntEAddress, 0u);
    bus_write_u8(NdsVramControl::VramCntHAddress, UINT8_C(0x9D));
    bus_write_u8(NdsVramControl::VramCntIAddress, UINT8_C(0x95));
    bus_write_u32(UINT32_C(0x06200000), UINT32_C(0x48484848));
    bus_write_u32(UINT32_C(0x06207FFC), UINT32_C(0x48494949));
    bus_write_u32(UINT32_C(0x06208000), UINT32_C(0x49494949));
    bus_write_u32(UINT32_C(0x0620BFFC), UINT32_C(0x494A4A4A));
    if (bus_read_u32(UINT32_C(0x06200000)) != UINT32_C(0x48484848) ||
        bus_read_u32(UINT32_C(0x06207FFC)) != UINT32_C(0x48494949) ||
        bus_read_u32(UINT32_C(0x06208000)) != UINT32_C(0x49494949) ||
        bus_read_u32(UINT32_C(0x0620BFFC)) != UINT32_C(0x494A4A4A)) {
        return fail("VRAM H/I Sub BG sizes, adjacency or ignored OFS failed");
    }
    bus_write_u8(NdsVramControl::VramCntHAddress, 0u);
    bus_write_u8(NdsVramControl::VramCntIAddress, UINT8_C(0x9E));
    bus_write_u32(UINT32_C(0x06600000), UINT32_C(0x494B4B4B));
    bus_write_u32(UINT32_C(0x06603FFC), UINT32_C(0x494C4C4C));
    if (bus_read_u32(UINT32_C(0x06600000)) != UINT32_C(0x494B4B4B) ||
        bus_read_u32(UINT32_C(0x06603FFC)) != UINT32_C(0x494C4C4C)) {
        return fail("VRAM I Sub OBJ 16KB window or ignored OFS failed");
    }

    if (oracis_bus_unbind_memory(&memory)) {
        return fail("memory unbind succeeded while VRAM controller remained bound");
    }
    if (!oracis_bus_unbind_vram_control(&control) ||
        !oracis_bus_unbind_memory(&memory) ||
        !oracis_arm_runtime_unbind_cpus(&arm9, &arm7)) {
        return fail("VRAM/GuestMemory/CPU unbinding failed");
    }

    std::puts("VRAM LCDC, main/sub BG/OBJ CPU mappings, byte-store rules and ARM7 C/D mapping passed");
    return 0;
}
