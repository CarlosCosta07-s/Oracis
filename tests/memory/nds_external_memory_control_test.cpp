#include "oracis/memory/guest_memory_abi.h"
#include "oracis/memory/nds_external_memory_control.hpp"
#include "oracis/recomp/runtime_arm.h"
#include <cstdint>
#include <cstdio>
namespace {
int fail(const char *message)
{
    std::fprintf(stderr, "nds_external_memory_control_test: %s\n", message);
    return 1;
}
}
int main()
{
    using oracis::memory::GuestMemory;
    using oracis::memory::NdsExternalMemoryControl;
    using oracis::memory::Status;
    GuestMemory memory;
    NdsExternalMemoryControl external_memory;
    ArmCpuState arm9{}, arm7{};
    arm9.cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7.cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (!oracis_arm_runtime_bind_cpus(&arm9, &arm7) ||
        !oracis_bus_bind_memory(&memory) ||
        !oracis_bus_bind_external_memory_control(&external_memory))
        return fail("bus binding failed");
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        bus_read_u16(NdsExternalMemoryControl::Address) != 0u)
        return fail("ARM9 EXMEMCNT initial state was not zero");

    /* Empty Slot-2: default 10-cycle N-access returns Addr/2 | FE08h. */
    if (bus_read_u32(NdsExternalMemoryControl::GbaSlotRomBase +
                     UINT32_C(0x80)) != UINT32_C(0x0041FE48) ||
        bus_read_u32(NdsExternalMemoryControl::GbaSlotSramBase) !=
            UINT32_MAX) {
        return fail("empty GBA Slot-2 ARM9 open-bus values were incorrect");
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7) ||
        bus_read_u32(NdsExternalMemoryControl::GbaSlotRomBase +
                     UINT32_C(0x80)) != 0u ||
        !oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("GBA Slot-2 ownership did not zero the deselected CPU bus");
    }

    /* Bit 14 enables synchronous main-memory mode; bits 7 and 11 grant
       GBA/Game Card access to ARM7. */
    bus_write_u16(NdsExternalMemoryControl::Address, UINT32_C(0x4880));
    if (bus_read_u16(NdsExternalMemoryControl::Address) != UINT32_C(0x4880))
        return fail("ARM9 EXMEMCNT shared-field write failed");
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7) ||
        bus_read_u16(NdsExternalMemoryControl::Address) != UINT32_C(0x4880))
        return fail("ARM7 EXMEMSTAT did not expose ARM9 shared fields");

    bus_write_u16(NdsExternalMemoryControl::Address, UINT32_C(0xFFFF));
    if (bus_read_u16(NdsExternalMemoryControl::Address) != UINT32_C(0x48FF))
        return fail("ARM7 local timing shadow write failed");
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        bus_read_u16(NdsExternalMemoryControl::Address) != UINT32_C(0x4880))
        return fail("ARM7 write changed ARM9 state");

    bus_write_u16(NdsExternalMemoryControl::Address, UINT32_C(0x8081));
    if (bus_read_u16(NdsExternalMemoryControl::Address) != UINT32_C(0x8081))
        return fail("ARM9 shared-field update failed");
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7) ||
        bus_read_u16(NdsExternalMemoryControl::Address) != UINT32_C(0x80FF))
        return fail("ARM7 local timing shadow was not preserved");

    if (external_memory.read(NDS_ARM9, NdsExternalMemoryControl::Address, 1u).status != Status::InvalidWidth ||
        external_memory.write(NDS_ARM9, NdsExternalMemoryControl::Address, 4u, 0u) != Status::InvalidWidth ||
        external_memory.read(NDS_ARM9, NdsExternalMemoryControl::Address + 2u, 2u).status != Status::Unmapped ||
        external_memory.read(static_cast<NdsCpu>(77), NdsExternalMemoryControl::Address, 2u).status != Status::InvalidCpu)
        return fail("invalid MMIO access was accepted");

    /* ARM9 alone changes Slot-2 ownership; ARM7 retains its local timing. */
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        external_memory.write(NDS_ARM9, NdsExternalMemoryControl::Address,
            2u, UINT32_C(0x4880)) != Status::Ok ||
        external_memory.write(NDS_ARM7, NdsExternalMemoryControl::Address,
            2u, 0u) != Status::Ok ||
        external_memory.read_gba_slot(NDS_ARM7,
            NdsExternalMemoryControl::GbaSlotRomBase + UINT32_C(0x80),
            2u).value != UINT32_C(0xFE48) ||
        external_memory.read_gba_slot(NDS_ARM9,
            NdsExternalMemoryControl::GbaSlotRomBase + UINT32_C(0x80),
            2u).value != 0u ||
        external_memory.read_gba_slot(NDS_ARM7,
            NdsExternalMemoryControl::GbaSlotSramBase, 4u).value !=
                UINT32_MAX ||
        external_memory.read_gba_slot(NDS_ARM7,
            NdsExternalMemoryControl::GbaSlotEnd, 2u).status !=
                Status::Unmapped ||
        external_memory.write_gba_slot(NDS_ARM7,
            NdsExternalMemoryControl::GbaSlotRomBase, 4u,
            UINT32_C(0x12345678)) != Status::Ok) {
        return fail("ARM7 Slot-2 ownership, SRAM open bus, or range checks failed");
    }

    if (!oracis_bus_unbind_external_memory_control(&external_memory) ||
        !oracis_bus_unbind_memory(&memory) ||
        !oracis_arm_runtime_unbind_cpus(&arm9, &arm7))
        return fail("bus teardown failed");
    std::puts("NDS EXMEMCNT/EXMEMSTAT ownership tests passed");
    return 0;
}
