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
        /*
         * Advance the devices in scanline-sized quanta. Geometry must reach a
         * line boundary before the display announces it: at VBlank, the
         * geometry engine is released only after the display's line-192 edge;
         * at frame wrap, the new frame's BG0 producer must see the completed
         * SWAP_BUFFERS result and all geometry work performed during VBlank.
         *
         * Splitting at every line boundary also gives scanline observers a
         * stable, synchronized point at which to schedule the 3D line buffer.
         */
        constexpr uint64_t frame_cycles =
            static_cast<uint64_t>(NdsDisplayControl::CyclesPerScanline) *
            NdsDisplayControl::ScanlinesPerFrame;
        constexpr uint64_t vblank_offset =
            static_cast<uint64_t>(NdsDisplayControl::CyclesPerScanline) *
            NdsDisplayControl::VisibleScanlines;
        uint64_t cursor = global_bus_cycles_;
        while (cursor < synchronized_bus_cycles) {
            const uint64_t line_offset =
                cursor % NdsDisplayControl::CyclesPerScanline;
            const uint64_t until_line_boundary =
                NdsDisplayControl::CyclesPerScanline - line_offset;
            const uint64_t step = std::min(
                synchronized_bus_cycles - cursor, until_line_boundary);

            timers_.advance_bus_cycles(step);
            if (game_card_ != nullptr) {
                game_card_->advance_bus_cycles(step);
            }
            if (geometry_ != nullptr) {
                geometry_->advance_bus_cycles(step);
            }
            if (spi_ != nullptr) {
                spi_->advance_bus_cycles(step);
            }
            display_.advance_bus_cycles(step);
            cursor += step;

            if (geometry_ != nullptr &&
                cursor % frame_cycles == vblank_offset) {
                geometry_->notify_vblank();
            }
        }
        global_bus_cycles_ = synchronized_bus_cycles;
    }

    return NdsClockStatus::Ok;
}

NdsClockStatus NdsSystemClock::report_idle_bus_cycles(
    NdsCpu cpu, uint64_t elapsed_bus_cycles) noexcept
{
    if (!valid_cpu(cpu)) return NdsClockStatus::InvalidCpu;

    const uint64_t core_cycles_per_bus_cycle =
        cpu == NDS_ARM9 ? UINT64_C(2) : UINT64_C(1);
    if (elapsed_bus_cycles >
        std::numeric_limits<uint64_t>::max() /
            core_cycles_per_bus_cycle) {
        return NdsClockStatus::TimeOverflow;
    }

    const uint64_t elapsed_core_cycles =
        elapsed_bus_cycles * core_cycles_per_bus_cycle;
    const unsigned index = cpu_index(cpu);
    if (elapsed_core_cycles >
        std::numeric_limits<uint64_t>::max() - cpu_cycles_[index]) {
        return NdsClockStatus::TimeOverflow;
    }

    /*
     * This does not synthesize guest instructions. The caller explicitly
     * reports elapsed bus time for a CPU whose execution is idle while its
     * clock domain continues to run.
     */
    return report_cpu_cycles(
        cpu, cpu_cycles_[index] + elapsed_core_cycles);
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
    display_.reset_timing();
    if(game_card_!=nullptr)game_card_->reset_transfer();
    if (spi_ != nullptr) spi_->reset_transfer();
}

} // namespace oracis::memory
