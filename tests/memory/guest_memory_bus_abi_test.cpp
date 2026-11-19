#include "oracis/memory/guest_memory.hpp"
#include "oracis/memory/guest_memory_abi.h"
#include "oracis/recomp/runtime_arm.h"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "guest_memory_bus_abi_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using oracis::memory::GuestMemory;
    using oracis::memory::Readable;
    using oracis::memory::Writable;
    using oracis::memory::Executable;
    using oracis::memory::Status;

    GuestMemory memory;
    const uint8_t permissions = static_cast<uint8_t>(
        Readable | Writable | Executable);
    if (memory.map_ram(0x02000000u, 0x1000u,
                       permissions, "ARM shared RAM") != Status::Ok) {
        return fail("RAM map failed");
    }

    ArmCpuState arm9{};
    ArmCpuState arm7{};
    arm9.cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7.cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (!oracis_arm_runtime_bind_cpus(&arm9, &arm7)) {
        return fail("CPU binding failed");
    }
    if (!oracis_bus_bind_memory(&memory)) {
        return fail("memory binding failed");
    }
    if (oracis_bus_bind_memory(nullptr)) {
        return fail("null memory binding must be rejected");
    }

    bus_write_u32(0x02000000u, UINT32_C(0x78563412));
    bus_write_u16(0x02000004u, UINT16_C(0xBEEF));
    bus_write_u8(0x02000006u, UINT8_C(0xA5));
    if (bus_read_u32(0x02000000u) != UINT32_C(0x78563412) ||
        bus_read_u16(0x02000004u) != UINT16_C(0xBEEF) ||
        bus_read_u8(0x02000006u) != UINT8_C(0xA5)) {
        return fail("bus width/endian transfer failed");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM7)) {
        return fail("ARM7 selection failed");
    }
    bus_write_u32(0x02000008u, UINT32_C(0xAABBCCDD));
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 selection failed");
    }
    if (bus_read_u32(0x02000008u) != UINT32_C(0xAABBCCDD)) {
        return fail("ARM9/ARM7 bus view did not share guest RAM");
    }

    if (oracis_bus_unbind_memory(&arm9)) {
        return fail("wrong object must not unbind guest memory");
    }
    if (!oracis_bus_unbind_memory(&memory)) {
        return fail("correct guest-memory unbind failed");
    }
    if (oracis_bus_unbind_memory(&memory)) {
        return fail("second unbind must fail");
    }

    std::puts("guest bus C ABI mapping and ARM9/ARM7 tests passed");
    return 0;
}
