/*
 * Oracis generated-bank call-return stack.
 *
 * Direct C calls between generated banks use the native host stack. This
 * guest return stack distinguishes a guest return from a branch that happens
 * to target another address in the caller's code.
 */
#include "oracis/recomp/runtime_arm.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr uint32_t kCallReturnStackCapacity = 1024u;

struct CpuCallReturnStack {
    std::array<uint32_t, kCallReturnStackCapacity> entries{};
    uint32_t depth = 0u;
};

std::array<CpuCallReturnStack, 2> g_call_return_stacks{};

unsigned active_cpu_index(const char *operation)
{
    if (oracis_active_cpu_state == nullptr) {
        std::fprintf(stderr,
            "[Oracis call stack] %s without an active CPU state\n",
            operation);
        std::abort();
    }
    if (g_nds_active == NDS_ARM9) return 0u;
    if (g_nds_active == NDS_ARM7) return 1u;

    std::fprintf(stderr,
        "[Oracis call stack] %s with invalid active CPU=%u\n",
        operation, static_cast<unsigned>(g_nds_active));
    std::abort();
}

[[noreturn]] void call_stack_overflow(uint32_t return_pc,
                                      unsigned cpu,
                                      uint32_t depth)
{
    std::fprintf(stderr,
        "[Oracis call stack] overflow: CPU=%u return=0x%08X "
        "PC=0x%08X depth=%u capacity=%u\n",
        cpu,
        return_pc,
        oracis_active_cpu_state ? oracis_active_cpu_state->R[15] : 0u,
        depth,
        kCallReturnStackCapacity);
    std::abort();
}

} // namespace

extern "C" void runtime_call_push_return(uint32_t return_pc)
{
    const unsigned cpu = active_cpu_index("runtime_call_push_return");
    CpuCallReturnStack &stack = g_call_return_stacks[cpu];
    if (stack.depth >= kCallReturnStackCapacity) {
        call_stack_overflow(return_pc, cpu, stack.depth);
    }

    const uint32_t pc = return_pc & ~UINT32_C(1);
    const uint32_t key =
        pc | ((oracis_active_cpu_state->cpsr & CPSR_T_BIT) != 0u ? 1u : 0u);
    stack.entries[stack.depth++] = key;
}

extern "C" int runtime_call_should_return(uint32_t target_pc)
{
    const unsigned cpu = active_cpu_index("runtime_call_should_return");
    CpuCallReturnStack &stack = g_call_return_stacks[cpu];

    const uint32_t pc = target_pc & ~UINT32_C(1);
    const uint32_t key =
        pc | ((oracis_active_cpu_state->cpsr & CPSR_T_BIT) != 0u ? 1u : 0u);

    for (uint32_t index = stack.depth; index != 0u; --index) {
        const uint32_t slot = index - 1u;
        if (stack.entries[slot] == key) {
            /* Drop the matched frame and any younger guest call frames. */
            stack.depth = slot;
            return 1;
        }
    }
    return 0;
}

extern "C" void runtime_call_cancel_return(uint32_t return_pc)
{
    const unsigned cpu = active_cpu_index("runtime_call_cancel_return");
    CpuCallReturnStack &stack = g_call_return_stacks[cpu];
    const uint32_t pc = return_pc & ~UINT32_C(1);

    if (stack.depth != 0u &&
        (stack.entries[stack.depth - 1u] & ~UINT32_C(1)) == pc) {
        --stack.depth;
    }
}

extern "C" uint32_t runtime_call_stack_depth(void)
{
    return g_call_return_stacks[active_cpu_index(
        "runtime_call_stack_depth")].depth;
}

extern "C" const uint32_t *runtime_call_stack_data(void)
{
    return g_call_return_stacks[active_cpu_index(
        "runtime_call_stack_data")].entries.data();
}

extern "C" void runtime_call_stack_restore(
    const uint32_t *entries,
    uint32_t depth)
{
    const unsigned cpu = active_cpu_index("runtime_call_stack_restore");
    CpuCallReturnStack &stack = g_call_return_stacks[cpu];
    if (depth > kCallReturnStackCapacity) depth = kCallReturnStackCapacity;
    if (depth != 0u && entries == nullptr) {
        std::fprintf(stderr,
            "[Oracis call stack] restore received null entries for depth=%u\n",
            depth);
        std::abort();
    }

    for (uint32_t index = 0u; index < depth; ++index) {
        stack.entries[index] = entries[index];
    }
    stack.depth = depth;
}
