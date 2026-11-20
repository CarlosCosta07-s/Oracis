/*
 * Synchronized Nintendo DS system clock.
 *
 * ARM9 advances at two core cycles per NDS bus cycle; ARM7 advances at one.
 * Peripheral time is committed only up to the slower CPU time watermark.
 */
#ifndef ORACIS_MEMORY_NDS_SYSTEM_CLOCK_HPP
#define ORACIS_MEMORY_NDS_SYSTEM_CLOCK_HPP

#include <array>
#include <cstdint>

#include "oracis/memory/nds_timer_control.hpp"
#include "oracis/memory/nds_display_control.hpp"

namespace oracis::memory {

enum class NdsClockStatus : uint8_t {
    Ok = 0,
    InvalidCpu,
    TimeRegression,
    TimeOverflow
};

class NdsSystemClock final {
public:
    NdsSystemClock(
        NdsTimerControl &timers,
        NdsDisplayControl &display) noexcept
        : timers_(timers), display_(display) {}

    NdsSystemClock(const NdsSystemClock &) = delete;
    NdsSystemClock &operator=(const NdsSystemClock &) = delete;

    /*
     * Report a monotonically increasing cumulative core-cycle count for one
     * CPU. The caller reports both CPUs in the same emulated time epoch.
     * ARM7's count is scaled by two half-bus ticks; ARM9's is already in
     * half-bus ticks. Timers advance only when both watermarks complete a
     * whole bus cycle.
     */
    NdsClockStatus report_cpu_cycles(
        NdsCpu cpu, uint64_t cumulative_cycles) noexcept;

    /* Advance a non-executing CPU by explicitly elapsed peripheral bus ticks. */
    NdsClockStatus report_idle_bus_cycles(
        NdsCpu cpu, uint64_t elapsed_bus_cycles) noexcept;

    uint64_t cpu_cycle_count(NdsCpu cpu) const noexcept;
    uint64_t global_bus_cycles() const noexcept { return global_bus_cycles_; }

    /* Resets synchronization counters; timer register state is not reset. */
    void reset() noexcept;

    NdsTimerControl &timer_control() const noexcept { return timers_; }
    NdsDisplayControl &display_control() const noexcept { return display_; }

private:
    static bool valid_cpu(NdsCpu cpu) noexcept;
    static unsigned cpu_index(NdsCpu cpu) noexcept;

    NdsTimerControl &timers_;
    NdsDisplayControl &display_;
    std::array<uint64_t, 2> cpu_cycles_{};
    uint64_t global_bus_cycles_ = 0u;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_SYSTEM_CLOCK_HPP */
