#include "oracis/memory/guest_memory.hpp"
#include "oracis/memory/guest_memory_abi.h"
#include "oracis/recomp/runtime_arm.h"
#include "oracis/recomp/static_dispatch_abi.h"

#include <cstdint>
#include <cstdio>

namespace {

unsigned validated_entry_calls = 0u;

void validated_entry(void)
{
    ++validated_entry_calls;
}

const uint8_t expected_zero_code[] = {0u, 0u, 0u, 0u};
const NdsStaticValidation code_validation = {
    0x02000010u, 4u, expected_zero_code, nullptr, 0u
};

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

    /* Dispatch validation is tied to this live GuestMemory instance. */
    oracis_static_dispatch_clear();
    const NdsDispatchEntry code_row[] = {
        {0x02000010u, 0u, validated_entry, &code_validation},
    };
    if (oracis_static_dispatch_register_bank(
            NDS_ARM9, code_row, 1u, "validated-main-ram") !=
        ORACIS_DISPATCH_REGISTER_OK) {
        return fail("validated code bank registration failed");
    }

    uint32_t lookup_status = UINT32_MAX;
    uint32_t candidate_count = 0u;
    const NdsDispatchEntry *inactive = nullptr;
    const NdsDispatchEntry *selected = oracis_static_dispatch_lookup(
        NDS_ARM9, 0x02000010u, arm9.cpsr,
        &lookup_status, &candidate_count, &inactive);
    if (selected != nullptr ||
        lookup_status != ORACIS_DISPATCH_LOOKUP_MISS ||
        candidate_count != 1u || inactive != &code_row[0]) {
        return fail("zero-filled RAM incorrectly passed dispatch validation");
    }

    bus_write_u32(0x02000010u, 0u);
    selected = oracis_static_dispatch_lookup(
        NDS_ARM9, 0x02000010u, arm9.cpsr,
        &lookup_status, &candidate_count, &inactive);
    if (selected != &code_row[0] ||
        lookup_status != ORACIS_DISPATCH_LOOKUP_FOUND) {
        return fail("materialized matching code bytes did not validate");
    }

    bus_write_u8(0x02000012u, 1u);
    selected = oracis_static_dispatch_lookup(
        NDS_ARM9, 0x02000010u, arm9.cpsr,
        &lookup_status, &candidate_count, &inactive);
    if (selected != nullptr ||
        lookup_status != ORACIS_DISPATCH_LOOKUP_MISS ||
        inactive != &code_row[0]) {
        return fail("modified code bytes remained dispatchable");
    }

    bus_write_u8(0x02000012u, 0u);
    selected = oracis_static_dispatch_lookup(
        NDS_ARM9, 0x02000010u, arm9.cpsr,
        &lookup_status, &candidate_count, &inactive);
    if (selected != &code_row[0]) {
        return fail("restoring validated bytes did not restore dispatch");
    }
    if (validated_entry_calls != 0u) {
        return fail("lookup unexpectedly executed a generated bank body");
    }

    if (oracis_bus_unbind_memory(&arm9)) {
        return fail("wrong object must not unbind guest memory");
    }
    if (!oracis_static_dispatch_unregister_bank(NDS_ARM9, code_row, 1u)) {
        return fail("validated code bank unregister failed");
    }
    oracis_static_dispatch_clear();
    if (!oracis_bus_unbind_memory(&memory)) {
        return fail("correct guest-memory unbind failed");
    }
    if (oracis_bus_unbind_memory(&memory)) {
        return fail("second unbind must fail");
    }

    std::puts("guest bus C ABI mapping and ARM9/ARM7 tests passed");
    return 0;
}
