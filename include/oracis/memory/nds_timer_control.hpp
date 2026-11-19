/*
 * Nintendo DS ARM9/ARM7 timer register block.
 *
 * Each processor owns four 16-bit timers. The machine scheduler advances
 * them explicitly in NDS bus-clock ticks; this class does not guess how
 * instruction cycles map to peripheral clock cycles.
 */
#ifndef ORACIS_MEMORY_NDS_TIMER_CONTROL_HPP
#define ORACIS_MEMORY_NDS_TIMER_CONTROL_HPP

#include <array>
#include <cstdint>

#include "oracis/memory/guest_memory.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"

namespace oracis::memory {

class NdsTimerControl final {
public:
    static constexpr uint32_t Timer0CounterAddress = UINT32_C(0x04000100);
    static constexpr uint32_t TimerCount = 4u;
    static constexpr uint32_t TimerRegisterBytes = TimerCount * 4u;

    explicit NdsTimerControl(
        GuestMemory &memory,
        NdsInterruptControl &interrupts) noexcept
        : memory_(memory), interrupts_(interrupts) {}

    NdsTimerControl(const NdsTimerControl &) = delete;
    NdsTimerControl &operator=(const NdsTimerControl &) = delete;

    ReadResult read(NdsCpu cpu,
                    uint32_t address,
                    uint32_t width) const noexcept;

    Status write(NdsCpu cpu,
                 uint32_t address,
                 uint32_t width,
                 uint32_t value) noexcept;

    /*
     * Advance one CPU's timers by elapsed NDS bus-clock ticks.
     * The input is peripheral/bus cycles, not ARM instruction cycles.
     * Timer overflow IRQ requests are latched in the paired interrupt block.
     */
    void advance_bus_cycles(NdsCpu cpu, uint64_t cycles) noexcept;

    /* Convert native CPU cycles to peripheral ticks. ARM9 is 2:1 with
       retained half-cycle; ARM7 is 1:1. */
    void advance_core_cycles(NdsCpu cpu, uint64_t cycles) noexcept;

    GuestMemory &backing_memory() const noexcept { return memory_; }
    NdsInterruptControl &interrupt_control() const noexcept { return interrupts_; }

private:
    struct TimerState {
        uint16_t reload = 0u;
        uint16_t counter = 0u;
        uint16_t control = 0u;
        uint64_t prescaler_remainder = 0u;
    };

    static bool valid_cpu(NdsCpu cpu) noexcept;
    static unsigned cpu_index(NdsCpu cpu) noexcept;

    void write_control(TimerState &timer,
                       unsigned timer_index,
                       uint16_t value) noexcept;

    static uint64_t advance_counter(
        TimerState &timer, uint64_t increments) noexcept;

    static uint64_t advance_prescaled(
        TimerState &timer, uint64_t bus_cycles) noexcept;

    GuestMemory &memory_;
    NdsInterruptControl &interrupts_;
    std::array<std::array<TimerState, TimerCount>, 2> timers_{};
    uint8_t arm9_core_cycle_remainder_ = 0u;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_TIMER_CONTROL_HPP */
