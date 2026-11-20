#include "oracis/memory/guest_memory_abi.h"
#include "oracis/memory/nds_interrupt_control.hpp"
#include "oracis/memory/nds_display_control.hpp"
#include "oracis/memory/nds_system_clock.hpp"
#include "oracis/memory/nds_timer_control.hpp"

#include <cstdint>
#include <cstdio>
#include <limits>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_system_clock_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using oracis::memory::GuestMemory;
    using oracis::memory::NdsClockStatus;
    using oracis::memory::NdsInterruptControl;
    using oracis::memory::NdsDisplayControl;
    using oracis::memory::NdsSystemClock;
    using oracis::memory::NdsTimerControl;

    GuestMemory memory;
    NdsInterruptControl interrupts(memory);
    NdsTimerControl timers(memory, interrupts);
    NdsDisplayControl display(interrupts);
    if (display.install() != oracis::memory::Status::Ok) {
        return fail("display timing install failed");
    }
    NdsSystemClock clock(timers, display);

    ArmCpuState arm9{};
    ArmCpuState arm7{};
    arm9.cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7.cpsr = ORACIS_ARM_MODE_SYSTEM;

    if (!oracis_arm_runtime_bind_cpus(&arm9, &arm7) ||
        !oracis_bus_bind_memory(&memory) ||
        !oracis_bus_bind_interrupt_control(&interrupts) ||
        !oracis_bus_bind_display_control(&display) ||
        !oracis_bus_bind_timer_control(&timers) ||
        !oracis_bus_bind_system_clock(&clock)) {
        return fail("system clock bus binding failed");
    }

    if (oracis_bus_bind_system_clock(&clock) ||
        oracis_bus_unbind_timer_control(&timers)) {
        return fail("duplicate clock bind or timer unbind was accepted");
    }

    constexpr uint32_t timer0_irq = UINT32_C(1) << 3u;
    constexpr uint32_t timer0_start_irq = UINT32_C(0x00C00000);
    constexpr uint32_t timer0_reload = UINT32_C(0x0000FFFE);

    const NdsCpu cpus[] = {NDS_ARM9, NDS_ARM7};
    for (const NdsCpu cpu : cpus) {
        if (timers.write(
                cpu, NdsTimerControl::Timer0CounterAddress, 4u,
                timer0_start_irq | timer0_reload) !=
                oracis::memory::Status::Ok) {
            return fail("Timer0 setup failed");
        }
    }

    if (oracis_bus_global_bus_cycles() != 0u)
        return fail("global bus clock did not start at zero");

    /* ARM9 alone cannot move shared time past an ARM7 watermark at zero. */
    if (!oracis_bus_report_cpu_cycles(NDS_ARM9, 3u) ||
        oracis_bus_global_bus_cycles() != 0u ||
        timers.read(
            NDS_ARM9, NdsTimerControl::Timer0CounterAddress, 2u).value !=
            0xFFFEu) {
        return fail("peripheral clock advanced ahead of ARM7");
    }

    /* One ARM7 cycle is one bus cycle; ARM9 has reported three half-ticks. */
    if (!oracis_bus_report_cpu_cycles(NDS_ARM7, 1u) ||
        oracis_bus_global_bus_cycles() != 1u ||
        timers.read(
            NDS_ARM9, NdsTimerControl::Timer0CounterAddress, 2u).value !=
            0xFFFFu ||
        timers.read(
            NDS_ARM7, NdsTimerControl::Timer0CounterAddress, 2u).value !=
            0xFFFFu) {
        return fail("first synchronized bus cycle did not advance both timer banks");
    }

    /* Preserve the incomplete half-bus phase until ARM9 reaches it. */
    if (!oracis_bus_report_cpu_cycles(NDS_ARM7, 2u) ||
        oracis_bus_global_bus_cycles() != 1u) {
        return fail("partial shared-clock phase was rounded up");
    }

    if (!oracis_bus_report_cpu_cycles(NDS_ARM9, 4u) ||
        oracis_bus_global_bus_cycles() != 2u ||
        timers.read(
            NDS_ARM9, NdsTimerControl::Timer0CounterAddress, 2u).value !=
            0xFFFEu ||
        timers.read(
            NDS_ARM7, NdsTimerControl::Timer0CounterAddress, 2u).value !=
            0xFFFEu ||
        interrupts.read(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value != timer0_irq ||
        interrupts.read(
            NDS_ARM7, NdsInterruptControl::IfAddress, 4u).value != timer0_irq) {
        return fail("second synchronized bus cycle did not overflow both timers");
    }

    /* Repeated reports and a fast CPU cannot double-advance the bus. */
    if (!oracis_bus_report_cpu_cycles(NDS_ARM9, 4u) ||
        !oracis_bus_report_cpu_cycles(NDS_ARM7, 2u) ||
        oracis_bus_global_bus_cycles() != 2u) {
        return fail("repeated CPU reports advanced global time twice");
    }

    if (clock.report_cpu_cycles(NDS_ARM9, 3u) !=
            NdsClockStatus::TimeRegression ||
        clock.report_cpu_cycles(static_cast<NdsCpu>(9), 0u) !=
            NdsClockStatus::InvalidCpu ||
        clock.report_cpu_cycles(
            NDS_ARM7,
            std::numeric_limits<uint64_t>::max() / 2u + 1u) !=
            NdsClockStatus::TimeOverflow ||
        clock.global_bus_cycles() != 2u) {
        return fail("clock regression, invalid CPU or overflow was accepted");
    }

    if (clock.cpu_cycle_count(NDS_ARM9) != 4u ||
        clock.cpu_cycle_count(NDS_ARM7) != 2u) {
        return fail("cumulative CPU cycle watermarks were not retained");
    }

    /*
     * ARM9 runs ahead while ARM7 is idle. Explicitly reporting three elapsed
     * bus ticks for ARM7 catches its clock watermark up without inventing
     * instruction executions.
     */
    if (clock.report_cpu_cycles(NDS_ARM9, 10u) != NdsClockStatus::Ok ||
        clock.global_bus_cycles() != 2u ||
        !oracis_bus_report_idle_bus_cycles(NDS_ARM7, 3u) ||
        clock.global_bus_cycles() != 5u ||
        clock.cpu_cycle_count(NDS_ARM7) != 5u ||
        timers.read(
            NDS_ARM9, NdsTimerControl::Timer0CounterAddress, 2u).value !=
            0xFFFFu ||
        timers.read(
            NDS_ARM7, NdsTimerControl::Timer0CounterAddress, 2u).value !=
            0xFFFFu) {
        return fail("ARM7 idle bus-time report did not synchronize the global clock");
    }

    /* The same facility accounts for ARM9 idle time at its 2:1 clock ratio. */
    if (clock.report_cpu_cycles(NDS_ARM7, 7u) != NdsClockStatus::Ok ||
        clock.global_bus_cycles() != 5u ||
        clock.report_idle_bus_cycles(NDS_ARM9, 1u) != NdsClockStatus::Ok ||
        clock.global_bus_cycles() != 6u ||
        clock.cpu_cycle_count(NDS_ARM9) != 12u ||
        timers.read(
            NDS_ARM9, NdsTimerControl::Timer0CounterAddress, 2u).value !=
            0xFFFEu ||
        timers.read(
            NDS_ARM7, NdsTimerControl::Timer0CounterAddress, 2u).value !=
            0xFFFEu) {
        return fail("ARM9 idle bus-time report did not use the 2:1 clock ratio");
    }

    if (clock.report_idle_bus_cycles(
            NDS_ARM7, std::numeric_limits<uint64_t>::max()) !=
            NdsClockStatus::TimeOverflow ||
        clock.global_bus_cycles() != 6u) {
        return fail("overflowing idle-time report changed the global clock");
    }


    /*
     * Display timing follows the same dual-core watermark as the timer bank.
     * ARM9 runs ahead; the display cannot advance until ARM7 catches up.
     */
    if (display.write(
            NDS_ARM9, NdsDisplayControl::DispstatAddress, 2u,
            UINT16_C(0x0010)) != oracis::memory::Status::Ok ||
        display.write(
            NDS_ARM7, NdsDisplayControl::DispstatAddress, 2u,
            UINT16_C(0x0010)) != oracis::memory::Status::Ok) {
        return fail("display HBlank interrupt setup failed");
    }
    if (clock.report_cpu_cycles(NDS_ARM9, 3224u) != NdsClockStatus::Ok ||
        clock.global_bus_cycles() != 7u ||
        clock.report_idle_bus_cycles(NDS_ARM7, 1599u) != NdsClockStatus::Ok ||
        clock.global_bus_cycles() != 1606u ||
        (display.read(
            NDS_ARM9, NdsDisplayControl::DispstatAddress, 2u).value & 2u) == 0u ||
        (display.read(
            NDS_ARM7, NdsDisplayControl::DispstatAddress, 2u).value & 2u) != 0u) {
        return fail("display scanline time did not follow the synchronized bus watermark");
    }
    if (clock.report_idle_bus_cycles(NDS_ARM9, 7u) != NdsClockStatus::Ok ||
        clock.report_idle_bus_cycles(NDS_ARM7, 7u) != NdsClockStatus::Ok ||
        clock.global_bus_cycles() != 1613u ||
        (display.read(
            NDS_ARM7, NdsDisplayControl::DispstatAddress, 2u).value & 2u) == 0u) {
        return fail("ARM7 HBlank status did not follow the global 1613-cycle boundary");
    }
    if (!oracis_bus_unbind_system_clock(&clock) ||
        !oracis_bus_unbind_timer_control(&timers) ||
        !oracis_bus_unbind_display_control(&display) ||
        !oracis_bus_unbind_interrupt_control(&interrupts) ||
        !oracis_bus_unbind_memory(&memory)) {
        return fail("system clock bus teardown failed");
    }

    clock.reset();
    if (clock.global_bus_cycles() != 0u ||
        clock.cpu_cycle_count(NDS_ARM9) != 0u ||
        clock.cpu_cycle_count(NDS_ARM7) != 0u) {
        return fail("clock reset did not clear synchronization state");
    }

    std::puts("NDS synchronized ARM9/ARM7 clock, timers and display scanlines passed");
    return 0;
}
