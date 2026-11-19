#include "oracis/memory/nds_timer_control.hpp"

namespace oracis::memory {
namespace {

constexpr uint16_t kPrescalerMask = UINT16_C(0x0003);
constexpr uint16_t kCascadeBit = UINT16_C(0x0004);
constexpr uint16_t kTimerIrqBit = UINT16_C(0x0040);
constexpr uint16_t kTimerEnableBit = UINT16_C(0x0080);
constexpr uint16_t kKnownControlBits =
    static_cast<uint16_t>(kPrescalerMask | kCascadeBit |
                          kTimerIrqBit | kTimerEnableBit);
constexpr uint64_t kCounterPeriod = UINT64_C(0x10000);
constexpr uint64_t kPrescalers[] = {
    UINT64_C(1), UINT64_C(64), UINT64_C(256), UINT64_C(1024)
};

bool is_timer_register_address(uint32_t address) noexcept
{
    return address >= NdsTimerControl::Timer0CounterAddress &&
           static_cast<uint64_t>(address) <
               static_cast<uint64_t>(NdsTimerControl::Timer0CounterAddress) +
                   NdsTimerControl::TimerRegisterBytes;
}

} // namespace

bool NdsTimerControl::valid_cpu(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 || cpu == NDS_ARM7;
}

unsigned NdsTimerControl::cpu_index(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 ? 0u : 1u;
}

ReadResult NdsTimerControl::read(
    NdsCpu cpu,
    uint32_t address,
    uint32_t width) const noexcept
{
    if (!is_timer_register_address(address)) {
        return {Status::Unmapped, 0u};
    }
    if (!valid_cpu(cpu)) return {Status::InvalidCpu, 0u};

    const uint32_t offset = address - Timer0CounterAddress;
    const unsigned timer_index = static_cast<unsigned>(offset / 4u);
    const uint32_t register_offset = offset % 4u;
    const TimerState &timer = timers_[cpu_index(cpu)][timer_index];

    if (register_offset == 0u && width == 2u) {
        return {Status::Ok, timer.counter};
    }
    if (register_offset == 0u && width == 4u) {
        return {
            Status::Ok,
            static_cast<uint32_t>(timer.counter) |
                (static_cast<uint32_t>(timer.control) << 16u)
        };
    }
    if (register_offset == 2u && width == 2u) {
        return {Status::Ok, timer.control};
    }
    return {Status::InvalidWidth, 0u};
}

void NdsTimerControl::write_control(
    TimerState &timer,
    unsigned timer_index,
    uint16_t value) noexcept
{
    const uint16_t old_control = timer.control;
    uint16_t new_control =
        static_cast<uint16_t>(value & kKnownControlBits);

    /* TM0 has no count-up input; the reserved bit reads back as zero. */
    if (timer_index == 0u) {
        new_control = static_cast<uint16_t>(new_control & ~kCascadeBit);
    }

    const bool was_enabled = (old_control & kTimerEnableBit) != 0u;
    const bool is_enabled = (new_control & kTimerEnableBit) != 0u;

    if (!was_enabled && is_enabled) {
        /* Reload is latched into the live counter on a 0 -> 1 start edge. */
        timer.counter = timer.reload;
        timer.prescaler_remainder = 0u;
    } else if (was_enabled && !is_enabled) {
        /* The stopped counter is frozen; sub-prescaler phase is discarded. */
        timer.prescaler_remainder = 0u;
    } else if (is_enabled &&
               ((old_control ^ new_control) &
                static_cast<uint16_t>(kPrescalerMask | kCascadeBit)) != 0u) {
        /* Avoid carrying a partial tick across a clock-source change. */
        timer.prescaler_remainder = 0u;
    }

    timer.control = new_control;
}

Status NdsTimerControl::write(
    NdsCpu cpu,
    uint32_t address,
    uint32_t width,
    uint32_t value) noexcept
{
    if (!is_timer_register_address(address)) return Status::Unmapped;
    if (!valid_cpu(cpu)) return Status::InvalidCpu;

    const uint32_t offset = address - Timer0CounterAddress;
    const unsigned timer_index = static_cast<unsigned>(offset / 4u);
    const uint32_t register_offset = offset % 4u;
    TimerState &timer = timers_[cpu_index(cpu)][timer_index];

    if (register_offset == 0u && width == 2u) {
        /* A low-halfword write changes reload, not the running counter. */
        timer.reload = static_cast<uint16_t>(value);
        return Status::Ok;
    }
    if (register_offset == 0u && width == 4u) {
        /*
         * One 32-bit operation updates reload and control together. If this
         * raises the enable bit, write_control observes the new reload value.
         */
        timer.reload = static_cast<uint16_t>(value);
        write_control(timer, timer_index,
                      static_cast<uint16_t>(value >> 16u));
        return Status::Ok;
    }
    if (register_offset == 2u && width == 2u) {
        write_control(timer, timer_index, static_cast<uint16_t>(value));
        return Status::Ok;
    }
    return Status::InvalidWidth;
}

uint64_t NdsTimerControl::advance_counter(
    TimerState &timer, uint64_t increments) noexcept
{
    if (increments == 0u) return 0u;

    const uint64_t until_first_overflow =
        kCounterPeriod - static_cast<uint64_t>(timer.counter);

    if (increments < until_first_overflow) {
        timer.counter = static_cast<uint16_t>(
            static_cast<uint64_t>(timer.counter) + increments);
        return 0u;
    }

    increments -= until_first_overflow;
    const uint64_t period_after_reload =
        kCounterPeriod - static_cast<uint64_t>(timer.reload);
    const uint64_t overflows =
        UINT64_C(1) + (increments / period_after_reload);
    const uint64_t remainder = increments % period_after_reload;

    timer.counter = static_cast<uint16_t>(
        static_cast<uint64_t>(timer.reload) + remainder);
    return overflows;
}

uint64_t NdsTimerControl::advance_prescaled(
    TimerState &timer, uint64_t bus_cycles) noexcept
{
    const uint64_t divisor =
        kPrescalers[timer.control & kPrescalerMask];

    const uint64_t whole_ticks = bus_cycles / divisor;
    const uint64_t partial_cycles = bus_cycles % divisor;
    const uint64_t accumulated =
        timer.prescaler_remainder + partial_cycles;

    timer.prescaler_remainder = accumulated % divisor;
    return whole_ticks + (accumulated / divisor);
}

void NdsTimerControl::advance_bus_cycles(
    NdsCpu cpu, uint64_t cycles) noexcept
{
    if (!valid_cpu(cpu)) return;

    auto &cpu_timers = timers_[cpu_index(cpu)];
    uint64_t overflows[TimerCount] = {};

    for (unsigned index = 0u; index < TimerCount; ++index) {
        TimerState &timer = cpu_timers[index];
        if ((timer.control & kTimerEnableBit) == 0u) continue;

        uint64_t increments = 0u;
        const bool cascade =
            index != 0u && (timer.control & kCascadeBit) != 0u;
        if (cascade) {
            increments = overflows[index - 1u];
        } else {
            increments = advance_prescaled(timer, cycles);
        }

        overflows[index] = advance_counter(timer, increments);
        if (overflows[index] != 0u &&
            (timer.control & kTimerIrqBit) != 0u) {
            const uint32_t irq_mask =
                UINT32_C(1) << (3u + index);
            (void)interrupts_.request_irq(cpu, irq_mask);
        }
    }
}

} // namespace oracis::memory
