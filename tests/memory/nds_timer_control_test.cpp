#include "oracis/memory/guest_memory_abi.h"
#include "oracis/memory/nds_interrupt_control.hpp"
#include "oracis/memory/nds_timer_control.hpp"
#include "oracis/recomp/runtime_arm.h"
#include "oracis/recomp/runtime_scheduler.h"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_timer_control_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using oracis::memory::GuestMemory;
    using oracis::memory::NdsInterruptControl;
    using oracis::memory::NdsTimerControl;
    using oracis::memory::Status;

    GuestMemory memory;
    NdsInterruptControl interrupts(memory);
    NdsTimerControl timers(memory, interrupts);

    constexpr uint32_t timer0_irq = UINT32_C(1) << 3u;
    constexpr uint32_t timer1_irq = UINT32_C(1) << 4u;
    constexpr uint32_t timer2_irq = UINT32_C(1) << 5u;

    constexpr uint32_t timer0_low = NdsTimerControl::Timer0CounterAddress;
    constexpr uint32_t timer0_high = timer0_low + 2u;
    constexpr uint32_t timer1_low = timer0_low + 4u;
    constexpr uint32_t timer1_high = timer1_low + 2u;
    constexpr uint32_t timer2_low = timer0_low + 8u;
    constexpr uint32_t timer2_high = timer2_low + 2u;

    if (timers.read(NDS_ARM9, timer0_low, 2u).value != 0u ||
        timers.read(NDS_ARM7, timer0_low, 2u).value != 0u ||
        timers.read(NDS_ARM9, timer0_high, 2u).value != 0u) {
        return fail("timer registers did not initialize to zero");
    }

    if (interrupts.write(
            NDS_ARM9, NdsInterruptControl::ImeAddress, 4u, 1u) != Status::Ok ||
        interrupts.write(
            NDS_ARM9, NdsInterruptControl::IeAddress, 4u,
            timer0_irq | timer1_irq | timer2_irq) != Status::Ok) {
        return fail("ARM9 interrupt setup failed");
    }

    /* Normal timer: start loads reload, then overflow reloads and latches IF. */
    if (timers.write(NDS_ARM9, timer0_low, 2u, 0xFFFEu) != Status::Ok ||
        timers.read(NDS_ARM9, timer0_low, 2u).value != 0u ||
        timers.write(NDS_ARM9, timer0_high, 2u, 0x00C0u) != Status::Ok ||
        timers.read(NDS_ARM9, timer0_low, 2u).value != 0xFFFEu) {
        return fail("timer 0 reload/start behavior failed");
    }

    timers.advance_bus_cycles(NDS_ARM9, 1u);
    if (timers.read(NDS_ARM9, timer0_low, 2u).value != 0xFFFFu ||
        interrupts.read(NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value != 0u) {
        return fail("timer 0 did not increment before overflow");
    }
    timers.advance_bus_cycles(NDS_ARM9, 1u);
    if (timers.read(NDS_ARM9, timer0_low, 2u).value != 0xFFFEu ||
        interrupts.read(NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value !=
            timer0_irq ||
        !interrupts.irq_asserted(NDS_ARM9)) {
        return fail("timer 0 overflow did not reload and request IRQ");
    }

    /* Updating reload while running must not overwrite the live counter. */
    if (interrupts.write(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u, timer0_irq) != Status::Ok) {
        return fail("timer 0 IRQ acknowledge failed");
    }
    timers.advance_bus_cycles(NDS_ARM9, 1u);
    if (timers.write(NDS_ARM9, timer0_low, 2u, 0x1234u) != Status::Ok ||
        timers.read(NDS_ARM9, timer0_low, 2u).value != 0xFFFFu) {
        return fail("reload write changed the running timer counter");
    }
    timers.advance_bus_cycles(NDS_ARM9, 1u);
    if (timers.read(NDS_ARM9, timer0_low, 2u).value != 0x1234u) {
        return fail("new reload was not applied at the next overflow");
    }

    /* Stopping freezes the counter; restarting loads the most recent reload. */
    if (timers.write(NDS_ARM9, timer0_high, 2u, 0x0040u) != Status::Ok) {
        return fail("timer 0 stop failed");
    }
    timers.advance_bus_cycles(NDS_ARM9, 1000u);
    if (timers.read(NDS_ARM9, timer0_low, 2u).value != 0x1234u ||
        timers.write(NDS_ARM9, timer0_high, 2u, 0x00C0u) != Status::Ok ||
        timers.read(NDS_ARM9, timer0_low, 2u).value != 0x1234u) {
        return fail("timer stop/freeze/restart semantics failed");
    }

    /* Prescaler /64 retains partial bus-clock cycles between advances. */
    if (timers.write(NDS_ARM9, timer0_high, 2u, 0x0041u) != Status::Ok ||
        timers.write(NDS_ARM9, timer0_low, 2u, 0xFFFFu) != Status::Ok ||
        timers.write(NDS_ARM9, timer0_high, 2u, 0x00C1u) != Status::Ok) {
        return fail("timer 0 prescaler setup failed");
    }
    timers.advance_bus_cycles(NDS_ARM9, 63u);
    if (timers.read(NDS_ARM9, timer0_low, 2u).value != 0xFFFFu) {
        return fail("timer /64 advanced before a full prescaler interval");
    }
    timers.advance_bus_cycles(NDS_ARM9, 1u);
    if (timers.read(NDS_ARM9, timer0_low, 2u).value != 0xFFFFu ||
        (interrupts.read(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value &
         timer0_irq) == 0u) {
        return fail("timer /64 did not overflow on the 64th bus cycle");
    }

    /* Cascade: timer 1 counts timer 0 overflows; timer 2 counts timer 1. */
    if (interrupts.write(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u, UINT32_MAX) != Status::Ok ||
        timers.write(NDS_ARM9, timer0_high, 2u, 0x0040u) != Status::Ok ||
        timers.write(NDS_ARM9, timer0_low, 2u, 0xFFFEu) != Status::Ok ||
        timers.write(NDS_ARM9, timer1_low, 2u, 0xFFFEu) != Status::Ok ||
        timers.write(NDS_ARM9, timer1_high, 2u, 0x00C4u) != Status::Ok ||
        timers.write(NDS_ARM9, timer2_low, 2u, 0xFFFFu) != Status::Ok ||
        timers.write(NDS_ARM9, timer2_high, 2u, 0x00C4u) != Status::Ok ||
        timers.write(NDS_ARM9, timer0_high, 2u, 0x0080u) != Status::Ok) {
        return fail("timer cascade setup failed");
    }

    timers.advance_bus_cycles(NDS_ARM9, 2u);
    if (timers.read(NDS_ARM9, timer1_low, 2u).value != 0xFFFFu ||
        timers.read(NDS_ARM9, timer2_low, 2u).value != 0xFFFFu) {
        return fail("first cascade edge was not propagated");
    }
    timers.advance_bus_cycles(NDS_ARM9, 2u);
    const uint32_t cascade_flags =
        interrupts.read(NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value;
    if (timers.read(NDS_ARM9, timer1_low, 2u).value != 0xFFFEu ||
        timers.read(NDS_ARM9, timer2_low, 2u).value != 0xFFFFu ||
        (cascade_flags & timer1_irq) == 0u ||
        (cascade_flags & timer2_irq) == 0u) {
        return fail("timer 1/2 cascaded overflow behavior failed");
    }

    /* ARM7 has a separate timer bank and clock advancement. */
    if (timers.write(NDS_ARM7, timer0_low, 2u, 0xABCDu) != Status::Ok ||
        timers.write(NDS_ARM7, timer0_high, 2u, 0x0080u) != Status::Ok ||
        timers.read(NDS_ARM7, timer0_low, 2u).value != 0xABCDu) {
        return fail("ARM7 timer state was not independent");
    }
    timers.advance_bus_cycles(NDS_ARM7, 1u);
    if (timers.read(NDS_ARM7, timer0_low, 2u).value != 0xABCEu ||
        timers.read(NDS_ARM9, timer0_low, 2u).value != 0xFFFEu) {
        return fail("ARM7 timer advancement affected ARM9 timer state");
    }

    if (timers.read(NDS_ARM9, timer0_low, 1u).status != Status::InvalidWidth ||
        timers.write(NDS_ARM9, timer0_high, 4u, 0u) != Status::InvalidWidth ||
        timers.read(NDS_ARM9, timer0_low + 0x20u, 2u).status != Status::Unmapped ||
        timers.read(static_cast<NdsCpu>(77), timer0_low, 2u).status !=
            Status::InvalidCpu) {
        return fail("invalid timer MMIO access or CPU was accepted");
    }

    /* The guest C bus ABI routes timer registers by the active CPU. */
    ArmCpuState arm9{};
    ArmCpuState arm7{};
    arm9.cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7.cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (!oracis_arm_runtime_bind_cpus(&arm9, &arm7) ||
        !oracis_bus_bind_memory(&memory) ||
        !oracis_bus_bind_interrupt_control(&interrupts) ||
        !oracis_bus_bind_timer_control(&timers)) {
        return fail("timer bus binding failed");
    }
    if (oracis_bus_bind_timer_control(&timers) ||
        oracis_bus_unbind_interrupt_control(&interrupts) ||
        oracis_bus_unbind_memory(&memory)) {
        return fail("duplicate or premature timer-related unbind was accepted");
    }

    oracis_scheduler_reset(NDS_ARM9);
    oracis_scheduler_reset(NDS_ARM7);

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9))
        return fail("ARM9 select failed for timer bus test");
    bus_write_u16(timer0_high, 0x0040u);
    bus_write_u32(timer0_low, UINT32_C(0x00C0FFFF));
    bus_write_u32(NdsInterruptControl::IfAddress, UINT32_MAX);
    if (bus_read_u32(timer0_low) != UINT32_C(0x00C0FFFF))
        return fail("combined reload/control 32-bit MMIO access failed");
    bus_write_u32(NdsInterruptControl::ImeAddress, 1u);
    bus_write_u32(NdsInterruptControl::IeAddress, timer0_irq);

    runtime_tick(1u);
    if (bus_read_u16(timer0_low) != 0xFFFFu ||
        bus_read_u32(NdsInterruptControl::IfAddress) != 0u)
        return fail("one ARM9 core cycle incorrectly advanced the bus timer");

    /*
     * Interleave the other CPU while ARM9 has a half-tick pending. ARM7 must
     * advance independently and must not discard ARM9's fractional phase.
     */
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7))
        return fail("ARM7 select failed during ARM9 fractional-cycle test");
    bus_write_u16(timer0_high, 0u);
    bus_write_u32(timer0_low, UINT32_C(0x0080BEE0));
    runtime_tick(1u);
    if (bus_read_u16(timer0_low) != 0xBEE1u ||
        bus_read_u32(NdsInterruptControl::IfAddress) != 0u)
        return fail("ARM7 tick failed during ARM9 fractional-cycle test");

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9))
        return fail("ARM9 reselection failed during fractional-cycle test");
    runtime_tick(1u);
    if (bus_read_u16(timer0_low) != 0xFFFFu ||
        (bus_read_u32(NdsInterruptControl::IfAddress) & timer0_irq) == 0u ||
        !interrupts.irq_asserted(NDS_ARM9))
        return fail("ARM9 half-tick was lost across ARM7 execution");

    if (!oracis_arm_runtime_select_cpu(NDS_ARM7))
        return fail("ARM7 select failed for timer bus test");
    bus_write_u16(timer0_high, 0u);
    bus_write_u32(timer0_low, UINT32_C(0x0080BEE0));
    if (bus_read_u16(timer0_low) != 0xBEE0u)
        return fail("ARM7 timer C bus state was not independent");
    runtime_tick(1u);
    if (bus_read_u16(timer0_low) != 0xBEE1u ||
        bus_read_u32(NdsInterruptControl::IfAddress) != 0u)
        return fail("one ARM7 core cycle did not advance a timer tick");

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        bus_read_u16(timer0_low) != 0xFFFFu) {
        return fail("ARM9 timer was altered by ARM7 timer advancement");
    }

    if (!oracis_bus_unbind_timer_control(&timers) ||
        !oracis_bus_unbind_interrupt_control(&interrupts) ||
        !oracis_bus_unbind_memory(&memory)) {
        return fail("timer/interrupt/memory bus teardown failed");
    }

    std::puts("NDS ARM9/ARM7 timer registers, prescalers, cascade and IRQ tests passed");
    return 0;
}
