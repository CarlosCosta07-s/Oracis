#include "oracis/recomp/runtime_arm.h"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "call_return_stack_test: %s\n", message);
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
        return fail("CPU-state binding failed");

    runtime_call_stack_restore(nullptr, 0u);
    if (runtime_call_stack_depth() != 0u)
        return fail("new ARM9 stack was not empty");

    /* Same address in the wrong instruction-set state is not a return. */
    runtime_call_push_return(0x02001000u);
    if (runtime_call_stack_depth() != 1u)
        return fail("ARM call push did not increase depth");
    arm9.cpsr |= CPSR_T_BIT;
    if (runtime_call_should_return(0x02001000u))
        return fail("ARM return matched while current CPU was in Thumb state");
    arm9.cpsr &= ~CPSR_T_BIT;
    if (!runtime_call_should_return(0x02001000u) ||
        runtime_call_stack_depth() != 0u) {
        return fail("matching ARM return did not pop its frame");
    }

    /* Interworking is represented by the state at the caller's return PC. */
    arm9.cpsr |= CPSR_T_BIT;
    runtime_call_push_return(0x02002001u);
    arm9.cpsr &= ~CPSR_T_BIT;
    if (runtime_call_should_return(0x02002000u))
        return fail("Thumb return frame matched in ARM state");
    arm9.cpsr |= CPSR_T_BIT;
    if (!runtime_call_should_return(0x02002001u))
        return fail("Thumb return frame did not match in Thumb state");
    arm9.cpsr &= ~CPSR_T_BIT;

    /* A non-local return drops the matching frame and younger frames. */
    runtime_call_push_return(0x02003000u);
    runtime_call_push_return(0x02004000u);
    runtime_call_push_return(0x02005000u);
    if (runtime_call_stack_depth() != 3u)
        return fail("nested call depth is incorrect");
    if (!runtime_call_should_return(0x02004000u) ||
        runtime_call_stack_depth() != 1u) {
        return fail("non-local return did not unwind younger frames");
    }

    /* Cancellation only removes the top frame and ignores the mode bit. */
    runtime_call_push_return(0x02006000u);
    runtime_call_push_return(0x02007001u);
    runtime_call_cancel_return(0x02006000u);
    if (runtime_call_stack_depth() != 3u)
        return fail("cancelling a non-top return removed a frame");
    runtime_call_cancel_return(0x02007000u);
    if (runtime_call_stack_depth() != 2u)
        return fail("cancelling the top return did not remove one frame");

    /* ARM7 maintains an independent stack while ARM9 remains suspended. */
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7))
        return fail("ARM7 selection failed");
    if (runtime_call_stack_depth() != 0u)
        return fail("ARM7 inherited ARM9 call-return depth");
    runtime_call_push_return(0x02381000u);
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9))
        return fail("ARM9 reselection failed");
    if (runtime_call_stack_depth() != 2u)
        return fail("ARM9 stack depth was not preserved across CPU switch");

    /* Save-state accessors return the selected CPU's live frame data. */
    const uint32_t *saved = runtime_call_stack_data();
    if (saved == nullptr || saved[0] != 0x02003000u ||
        saved[1] != 0x02006000u) {
        return fail("call-return stack data accessor returned wrong entries");
    }
    const uint32_t replacement[] = {0x02008000u, 0x02009001u};
    runtime_call_stack_restore(replacement, 2u);
    arm9.cpsr |= CPSR_T_BIT;
    if (runtime_call_stack_depth() != 2u ||
        !runtime_call_should_return(0x02009001u) ||
        runtime_call_stack_depth() != 1u) {
        return fail("restored Thumb return did not match the top frame");
    }
    arm9.cpsr &= ~CPSR_T_BIT;
    if (!runtime_call_should_return(0x02008000u) ||
        runtime_call_stack_depth() != 0u) {
        return fail("restored outer frame did not match after the inner return");
    }

    std::puts("per-CPU generated-bank call-return stack tests passed");
    return 0;
}
