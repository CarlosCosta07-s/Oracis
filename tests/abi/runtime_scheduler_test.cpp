#include "oracis/recomp/runtime_arm.h"
#include "oracis/recomp/runtime_scheduler.h"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "runtime_scheduler_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    ArmCpuState arm9{};
    ArmCpuState arm7{};
    arm9.cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7.cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (!oracis_arm_runtime_bind_cpus(&arm9, &arm7))
        return fail("CPU bind failed");

    g_insn_count[NDS_ARM9] = UINT64_C(123);
    g_insn_count[NDS_ARM7] = UINT64_C(456);
    oracis_scheduler_reset(NDS_ARM9);
    if (g_insn_count[NDS_ARM9] != 0u ||
        g_insn_count[NDS_ARM7] != UINT64_C(456)) {
        return fail("ARM9 scheduler reset did not clear only its instruction counter");
    }
    oracis_scheduler_reset(NDS_ARM7);
    if (g_insn_count[NDS_ARM7] != 0u || g_insn_hook_armed != 0u) {
        return fail("ARM7 scheduler reset or default hook state is incorrect");
    }

    if (!oracis_scheduler_begin_slice(NDS_ARM9, 10u))
        return fail("ARM9 slice configuration failed");
    runtime_tick(4u);
    if (runtime_should_yield() || runtime_slice_yield())
        return fail("ARM9 yielded before its cycle budget");
    runtime_tick(6u);
    if (!runtime_should_yield() || !runtime_slice_yield() ||
        !runtime_unwinding() ||
        oracis_scheduler_cycle_count(NDS_ARM9) != 10u) {
        return fail("ARM9 did not enter unwind after its cycle deadline");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM7))
        return fail("ARM7 selection failed");
    if (!oracis_scheduler_begin_slice(NDS_ARM7, 5u))
        return fail("ARM7 slice configuration failed");
    runtime_tick(5u);
    if (!runtime_should_yield() || !runtime_unwinding() ||
        oracis_scheduler_cycle_count(NDS_ARM7) != 5u) {
        return fail("ARM7 did not enter unwind when its budget expired");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9))
        return fail("ARM9 reselection failed");
    if (!runtime_should_yield())
        return fail("ARM9 yield state was lost while ARM7 was active");

    if (!oracis_scheduler_begin_slice(NDS_ARM9, 20u))
        return fail("new ARM9 slice configuration failed");
    if (runtime_should_yield() || runtime_unwinding())
        return fail("begin_slice did not clear prior yield/unwind state");
    runtime_tick(19u);
    if (runtime_should_yield())
        return fail("ARM9 yielded before the new deadline");
    runtime_tick(1u);
    if (!runtime_should_yield() || !runtime_unwinding())
        return fail("ARM9 did not enter unwind at the new deadline");
    oracis_scheduler_end_slice(NDS_ARM9);
    if (runtime_should_yield() || runtime_unwinding())
        return fail("ARM9 unwind flag survived the host slice boundary");

    if (!oracis_scheduler_request_yield(NDS_ARM7))
        return fail("explicit ARM7 yield request failed");
    if (!oracis_scheduler_set_unwinding(NDS_ARM7, 1))
        return fail("ARM7 unwind request failed");
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7))
        return fail("ARM7 reselection failed");
    if (!runtime_should_yield() || !runtime_unwinding())
        return fail("ARM7 explicit yield/unwind state was not observed");

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9))
        return fail("ARM9 selection failed");
    if (runtime_unwinding())
        return fail("ARM7 unwind flag leaked into ARM9");
    if (!oracis_scheduler_set_unwinding(NDS_ARM9, 1) ||
        !runtime_unwinding()) {
        return fail("ARM9 unwind state was not observed");
    }

    oracis_scheduler_end_slice(NDS_ARM9);
    if (runtime_should_yield() || runtime_unwinding())
        return fail("end_slice did not clear yield and unwind after host return");

    if (!oracis_scheduler_begin_slice(NDS_ARM9, 0u) ||
        !runtime_should_yield() || !runtime_unwinding()) {
        return fail("zero-budget slice must yield and mark stack unwind immediately");
    }
    oracis_scheduler_end_slice(NDS_ARM9);
    if (runtime_unwinding())
        return fail("zero-budget slice left the CPU in unwind state after end");

    if (oracis_scheduler_begin_slice(static_cast<NdsCpu>(9), 1u) ||
        oracis_scheduler_request_yield(static_cast<NdsCpu>(9)) ||
        oracis_scheduler_set_unwinding(static_cast<NdsCpu>(9), 1)) {
        return fail("invalid CPU identifier was accepted");
    }

    std::puts("cooperative ARM9/ARM7 slice-control tests passed");
    return 0;
}
