#include "oracis/recomp/runtime_arm.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "runtime_event_trace_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    static_assert(sizeof(OracisRuntimeEvent) == 32u);
    static_assert(ORACIS_RUNTIME_EVENT_CAPACITY == 512u);

    ArmCpuState arm9{};
    ArmCpuState arm7{};
    arm9.cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7.cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (!oracis_arm_runtime_bind_cpus(&arm9, &arm7)) {
        return fail("CPU binding failed");
    }
    if (!oracis_runtime_event_clear(NDS_ARM9) ||
        !oracis_runtime_event_clear(NDS_ARM7)) {
        return fail("event queues could not be cleared");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 selection failed");
    }
    runtime_trace_event(
        RUNTIME_TRACE_MEM_WRITE,
        UINT32_C(0x02000100),
        UINT32_C(0x04000000),
        UINT32_C(0x12345678),
        4u);
    arm9.cpsr |= CPSR_T_BIT;
    runtime_live_transfer(
        UINT32_C(0x02000200),
        UINT32_C(0x02001001),
        NDS_LIVE_TRANSFER_BLX_REG);

    if (oracis_runtime_event_available(NDS_ARM9) != 2u ||
        oracis_runtime_event_available(NDS_ARM7) != 0u ||
        oracis_runtime_event_dropped(NDS_ARM9) != 0u) {
        return fail("event queues were not isolated by CPU");
    }

    std::array<OracisRuntimeEvent, 4> events{};
    uint64_t cursor = 0u;
    const uint32_t read_count = oracis_runtime_event_read(
        NDS_ARM9, &cursor, events.data(),
        static_cast<uint32_t>(events.size()));
    if (read_count != 2u || cursor != 3u ||
        events[0].sequence != 1u ||
        events[0].kind != RUNTIME_TRACE_MEM_WRITE ||
        events[0].pc != UINT32_C(0x02000100) ||
        events[0].addr != UINT32_C(0x04000000) ||
        events[0].value != UINT32_C(0x12345678) ||
        events[0].aux != 4u ||
        events[1].sequence != 2u ||
        events[1].kind != RUNTIME_TRACE_LIVE_TRANSFER ||
        events[1].pc != UINT32_C(0x02000200) ||
        events[1].addr != UINT32_C(0x02001001) ||
        events[1].value != NDS_LIVE_TRANSFER_BLX_REG ||
        events[1].aux != 1u) {
        return fail("event records lost fields or were reordered");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM7)) {
        return fail("ARM7 selection failed");
    }
    runtime_trace_event(
        RUNTIME_TRACE_SWI, UINT32_C(0x02380010), 0u, 0x06u, 0u);
    cursor = 0u;
    if (oracis_runtime_event_read(
            NDS_ARM7, &cursor, events.data(), 4u) != 1u ||
        events[0].kind != RUNTIME_TRACE_SWI ||
        events[0].pc != UINT32_C(0x02380010) ||
        oracis_runtime_event_available(NDS_ARM9) != 2u) {
        return fail("ARM7 event queue interfered with ARM9 history");
    }

    if (!oracis_runtime_event_clear(NDS_ARM9) ||
        oracis_runtime_event_available(NDS_ARM9) != 0u ||
        oracis_runtime_event_dropped(NDS_ARM9) != 0u) {
        return fail("clear did not reset the selected CPU queue");
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 reselection failed");
    }

    for (uint32_t index = 0u;
         index < ORACIS_RUNTIME_EVENT_CAPACITY + 7u;
         ++index) {
        runtime_trace_event(
            RUNTIME_TRACE_BRANCH,
            UINT32_C(0x02010000) + index * 4u,
            UINT32_C(0x02020000) + index * 4u,
            index,
            0u);
    }
    if (oracis_runtime_event_available(NDS_ARM9) !=
            ORACIS_RUNTIME_EVENT_CAPACITY ||
        oracis_runtime_event_dropped(NDS_ARM9) != 7u) {
        return fail("bounded ring did not count overwritten events");
    }

    std::array<OracisRuntimeEvent, ORACIS_RUNTIME_EVENT_CAPACITY> retained{};
    cursor = 1u;
    const uint32_t retained_count = oracis_runtime_event_read(
        NDS_ARM9, &cursor, retained.data(),
        static_cast<uint32_t>(retained.size()));
    if (retained_count != ORACIS_RUNTIME_EVENT_CAPACITY ||
        retained.front().sequence != 8u ||
        retained.front().value != 7u ||
        retained.back().sequence !=
            ORACIS_RUNTIME_EVENT_CAPACITY + 7u ||
        retained.back().value != ORACIS_RUNTIME_EVENT_CAPACITY + 6u ||
        cursor != ORACIS_RUNTIME_EVENT_CAPACITY + 8u) {
        return fail("overflow did not retain the newest ordered events");
    }

    /* A stale reader cursor advances to the oldest event still available. */
    cursor = 1u;
    if (oracis_runtime_event_read(
            NDS_ARM9, &cursor, events.data(), 1u) != 1u ||
        cursor != 9u || events[0].sequence != 8u) {
        return fail("stale reader cursor did not recover after overflow");
    }

    if (oracis_runtime_event_available(static_cast<NdsCpu>(77)) != 0u ||
        oracis_runtime_event_dropped(static_cast<NdsCpu>(77)) != 0u ||
        oracis_runtime_event_clear(static_cast<NdsCpu>(77)) != 0) {
        return fail("invalid CPU was accepted by event API");
    }

    if (!oracis_arm_runtime_unbind_cpus(&arm9, &arm7)) {
        return fail("CPU unbinding failed");
    }

    std::puts("bounded per-CPU runtime trace tests passed");
    return 0;
}
