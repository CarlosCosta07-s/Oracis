#include "oracis/memory/nds_system_clock.hpp"

#include <algorithm>
#include <limits>

namespace oracis::memory {

bool NdsSystemClock::valid_cpu(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 || cpu == NDS_ARM7;
}

unsigned NdsSystemClock::cpu_index(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 ? 0u : 1u;
}

NdsClockStatus NdsSystemClock::report_cpu_cycles(
    NdsCpu cpu, uint64_t cumulative_cycles) noexcept
{
    if (!valid_cpu(cpu)) return NdsClockStatus::InvalidCpu;

    const unsigned index = cpu_index(cpu);
    if (cumulative_cycles < cpu_cycles_[index]) {
        return NdsClockStatus::TimeRegression;
    }

    const uint64_t arm9_cycles =
        cpu == NDS_ARM9 ? cumulative_cycles : cpu_cycles_[0];
    const uint64_t arm7_cycles =
        cpu == NDS_ARM7 ? cumulative_cycles : cpu_cycles_[1];

    if (arm7_cycles > std::numeric_limits<uint64_t>::max() / 2u) {
        return NdsClockStatus::TimeOverflow;
    }

    /*
     * Common unit: half of one peripheral bus cycle.
     * ARM9: 1 core cycle = 1 half-bus tick.
     * ARM7: 1 core cycle = 2 half-bus ticks.
     *
     * min() is a rendezvous watermark: it prevents peripheral time from
     * moving ahead of either CPU's reported emulated time.
     */
    const uint64_t arm7_half_bus_ticks = arm7_cycles * 2u;
    const uint64_t synchronized_half_ticks =
        std::min(arm9_cycles, arm7_half_bus_ticks);
    const uint64_t synchronized_bus_cycles =
        synchronized_half_ticks / 2u;

    if (synchronized_bus_cycles < global_bus_cycles_) {
        return NdsClockStatus::TimeRegression;
    }

    const uint64_t elapsed_bus_cycles =
        synchronized_bus_cycles - global_bus_cycles_;

    /* Commit only after all validation has completed. */
    cpu_cycles_[index] = cumulative_cycles;
    if (elapsed_bus_cycles != 0u) {
        timers_.advance_bus_cycles(elapsed_bus_cycles);
        global_bus_cycles_ = synchronized_bus_cycles;
    }

    return NdsClockStatus::Ok;
}

uint64_t NdsSystemClock::cpu_cycle_count(NdsCpu cpu) const noexcept
{
    if (!valid_cpu(cpu)) return 0u;
    return cpu_cycles_[cpu_index(cpu)];
}

void NdsSystemClock::reset() noexcept
{
    cpu_cycles_ = {};
    global_bus_cycles_ = 0u;
}

} // namespace oracis::memory
