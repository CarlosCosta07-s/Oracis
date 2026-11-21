#include "oracis/recomp/runtime_scheduler.h"

#include "oracis/recomp/runtime_arm.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>

extern "C" {
uint64_t g_insn_count[2] = {0u, 0u};
uint32_t g_insn_hook_armed = 0u;
}

namespace {

struct CpuSliceState {
    uint64_t cycles = 0u;
    uint64_t deadline = 0u;
    bool slice_active = false;
    bool yield_requested = false;
    bool unwinding = false;
};

std::array<CpuSliceState, 2> g_cpu_slice{};
bool valid_cpu(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 || cpu == NDS_ARM7;
}

unsigned cpu_index(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 ? 0u : 1u;
}

[[noreturn]] void scheduler_failure(const char *operation,
                                    const char *reason)
{
    std::fprintf(stderr,
        "[Oracis scheduler] %s failed: %s CPU=%u PC=0x%08X\n",
        operation,
        reason,
        static_cast<unsigned>(g_nds_active),
        oracis_active_cpu_state
            ? oracis_active_cpu_state->R[15] : 0u);
    std::abort();
}

CpuSliceState &active_slice_state(const char *operation)
{
    if (oracis_active_cpu_state == nullptr || !valid_cpu(g_nds_active)) {
        scheduler_failure(operation, "no active CPU state;");
    }
    return g_cpu_slice[cpu_index(g_nds_active)];
}

bool should_yield(const CpuSliceState &state) noexcept
{
    return state.yield_requested ||
           (state.slice_active && state.cycles >= state.deadline);
}

bool poll_yield_and_begin_unwind(CpuSliceState &state) noexcept
{
    const bool yield = should_yield(state);
    if (yield) {
        /*
         * Generated C frames unwind back to the host after a cooperative
         * preemption. The guest call/return stack must therefore preserve
         * pending call frames until the host closes this slice.
         */
        state.unwinding = true;
    }
    return yield;
}

} // namespace

extern "C" int oracis_scheduler_begin_slice(
    NdsCpu cpu,
    uint64_t cycle_budget)
{
    if (!valid_cpu(cpu)) return 0;

    CpuSliceState &state = g_cpu_slice[cpu_index(cpu)];
    if (cycle_budget >
        std::numeric_limits<uint64_t>::max() - state.cycles) {
        return 0;
    }

    state.deadline = state.cycles + cycle_budget;
    state.slice_active = true;
    state.yield_requested = cycle_budget == 0u;
    state.unwinding = false;
    return 1;
}

extern "C" void oracis_scheduler_end_slice(NdsCpu cpu)
{
    if (!valid_cpu(cpu)) return;
    CpuSliceState &state = g_cpu_slice[cpu_index(cpu)];
    state.slice_active = false;
    state.yield_requested = false;
    /* The generated C stack has returned to the host; unwind is over. */
    state.unwinding = false;
}

extern "C" int oracis_scheduler_request_yield(NdsCpu cpu)
{
    if (!valid_cpu(cpu)) return 0;
    g_cpu_slice[cpu_index(cpu)].yield_requested = true;
    return 1;
}

extern "C" int oracis_scheduler_set_unwinding(
    NdsCpu cpu,
    int unwinding)
{
    if (!valid_cpu(cpu)) return 0;
    g_cpu_slice[cpu_index(cpu)].unwinding = unwinding != 0;
    return 1;
}

extern "C" uint64_t oracis_scheduler_cycle_count(NdsCpu cpu)
{
    if (!valid_cpu(cpu)) return 0u;
    return g_cpu_slice[cpu_index(cpu)].cycles;
}

extern "C" void oracis_scheduler_reset(NdsCpu cpu)
{
    if (!valid_cpu(cpu)) return;
    g_cpu_slice[cpu_index(cpu)] = CpuSliceState{};
    g_insn_count[cpu_index(cpu)] = 0u;
}

extern "C" void runtime_tick(uint32_t cycles)
{
    CpuSliceState &state = active_slice_state("runtime_tick");
    if (cycles >
        std::numeric_limits<uint64_t>::max() - state.cycles) {
        scheduler_failure("runtime_tick", "cycle counter overflow;");
    }

    state.cycles += cycles;

    if (state.slice_active && state.cycles >= state.deadline) {
        state.yield_requested = true;
    }

    /*
     * Generated NDS banks retire instructions through runtime_tick. Accept an
     * IRQ at this same instruction boundary, even when the completed tick has
     * reached the slice deadline; the exception handler is part of the guest
     * instruction stream and the cooperative unwind still follows normally.
     */
    if (!state.unwinding && oracis_active_cpu_state != nullptr) {
        runtime_irq(oracis_active_cpu_state->R[15]);
    }
}

extern "C" bool runtime_should_yield(void)
{
    return poll_yield_and_begin_unwind(
        active_slice_state("runtime_should_yield"));
}

extern "C" bool runtime_slice_yield(void)
{
    return poll_yield_and_begin_unwind(
        active_slice_state("runtime_slice_yield"));
}

extern "C" bool runtime_unwinding(void)
{
    return active_slice_state("runtime_unwinding").unwinding;
}

extern "C" void runtime_insn_slow(void)
{
    std::fprintf(stderr,
        "[Oracis runtime] instruction slow hook is armed without a "
        "registered tracing consumer (CPU=%u, PC=0x%08X)\\n",
        static_cast<unsigned>(g_nds_active),
        oracis_active_cpu_state ? oracis_active_cpu_state->R[15] : 0u);
    std::abort();
}
