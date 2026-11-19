#include "oracis/machine/nds_machine.hpp"

#include "oracis/memory/guest_memory_abi.h"
#include "oracis/recomp/runtime_arm.h"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_machine_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using oracis::machine::NdsMachine;
    using oracis::machine::NdsMachineStatus;
    using oracis::memory::NdsInterruptControl;
    using oracis::memory::NdsKeypadControl;
    using oracis::memory::NdsTimerControl;
    using oracis::memory::SharedWramMode;

    NdsMachine machine(SharedWramMode::Arm9All32K);
    if (machine.initialize() != NdsMachineStatus::Ok ||
        !machine.initialized()) {
        return fail("machine initialization and device binding failed");
    }

    if (oracis_arm_runtime_current_state() != machine.cpu_state(NDS_ARM9) ||
        g_nds_active != NDS_ARM9 ||
        machine.cpu_state(NDS_ARM9)->cpsr != ORACIS_ARM_MODE_SYSTEM ||
        machine.cpu_state(NDS_ARM7)->cpsr != ORACIS_ARM_MODE_SYSTEM) {
        return fail("machine did not bind and initialize both CPU states");
    }

    if (machine.guest_memory().region_count() == 0u ||
        machine.guest_memory().write(
            NDS_ARM9, UINT32_C(0x02000000), 4u,
            UINT32_C(0x12345678)) != oracis::memory::Status::Ok ||
        machine.guest_memory().read(
            NDS_ARM7, UINT32_C(0x02000000), 4u).value !=
            UINT32_C(0x12345678)) {
        return fail("retail main RAM was not installed as shared memory");
    }

    constexpr uint32_t timer_irq = UINT32_C(1) << 3u;
    constexpr uint32_t timer_start_irq = UINT32_C(0x00C00000);
    constexpr uint32_t timer_reload = UINT32_C(0x0000FFFE);
    const NdsCpu cpus[] = {NDS_ARM9, NDS_ARM7};
    for (const NdsCpu cpu : cpus) {
        if (machine.timer_control().write(
                cpu, NdsTimerControl::Timer0CounterAddress, 4u,
                timer_start_irq | timer_reload) !=
                oracis::memory::Status::Ok) {
            return fail("timer setup failed");
        }
    }

    /* A CPU slice now reports its cumulative count at the end boundary. */
    if (machine.begin_cpu_slice(NDS_ARM9, 2u) != NdsMachineStatus::Ok ||
        machine.begin_cpu_slice(NDS_ARM9, 1u) !=
            NdsMachineStatus::SliceAlreadyActive ||
        machine.begin_cpu_slice(NDS_ARM7, 1u) !=
            NdsMachineStatus::SliceAlreadyActive ||
        machine.report_idle_bus_cycles(NDS_ARM7, 1u) !=
            NdsMachineStatus::SliceAlreadyActive) {
        return fail("machine allowed overlapping slices or idle-time accounting");
    }
    runtime_tick(2u);
    if (machine.end_cpu_slice(NDS_ARM9) != NdsMachineStatus::Ok ||
        machine.global_bus_cycles() != 0u) {
        return fail("ARM9-only progress advanced shared time past ARM7");
    }

    if (machine.begin_cpu_slice(NDS_ARM7, 1u) != NdsMachineStatus::Ok) {
        return fail("ARM7 slice begin failed");
    }
    runtime_tick(1u);
    if (machine.end_cpu_slice(NDS_ARM7) != NdsMachineStatus::Ok ||
        machine.global_bus_cycles() != 1u ||
        machine.timer_control().read(
            NDS_ARM9, NdsTimerControl::Timer0CounterAddress, 2u).value !=
            0xFFFFu ||
        machine.timer_control().read(
            NDS_ARM7, NdsTimerControl::Timer0CounterAddress, 2u).value !=
            0xFFFFu) {
        return fail("slice-boundary cycle reports did not advance both timer banks");
    }

    /* Both domains explicitly report idle bus time before the next tick commits. */
    if (machine.report_idle_bus_cycles(NDS_ARM9, 1u) !=
            NdsMachineStatus::Ok ||
        machine.report_idle_bus_cycles(NDS_ARM7, 1u) !=
            NdsMachineStatus::Ok ||
        machine.global_bus_cycles() != 2u ||
        machine.timer_control().read(
            NDS_ARM9, NdsTimerControl::Timer0CounterAddress, 2u).value !=
            0xFFFEu ||
        machine.timer_control().read(
            NDS_ARM7, NdsTimerControl::Timer0CounterAddress, 2u).value !=
            0xFFFEu ||
        (machine.interrupt_control().read(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value &
            timer_irq) == 0u ||
        (machine.interrupt_control().read(
            NDS_ARM7, NdsInterruptControl::IfAddress, 4u).value &
            timer_irq) == 0u) {
        return fail("idle-time watermark did not advance timers and latch IRQs");
    }

    /* Host key state reaches the bound MMIO device and its IRQ controller. */
    if (machine.keypad_control().write(
            NDS_ARM9, NdsKeypadControl::KeyControlAddress, 2u,
            NdsKeypadControl::IrqEnableMask | 1u) !=
            oracis::memory::Status::Ok) {
        return fail("ARM9 KEYCNT setup failed");
    }
    oracis_host_set_keypad_state(1u);
    if (machine.keypad_control().read(
            NDS_ARM9, NdsKeypadControl::KeyInputAddress, 2u).value !=
            UINT32_C(0x03FE) ||
        (machine.interrupt_control().read(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value &
            NdsKeypadControl::KeypadIrqMask) == 0u) {
        return fail("host keypad state did not reach KEYINPUT and keypad IRQ");
    }

    {
        NdsMachine competing_machine(SharedWramMode::Arm9All32K);
        if (competing_machine.initialize() !=
                NdsMachineStatus::RuntimeAlreadyBound) {
            return fail("a second machine was allowed to replace live CPU states");
        }
    }
    if (machine.global_bus_cycles() != 2u) {
        return fail("a rejected competing machine disturbed the active scheduler");
    }

    if (machine.shutdown() != NdsMachineStatus::Ok ||
        machine.initialized() ||
        oracis_arm_runtime_current_state() != nullptr ||
        oracis_bus_global_bus_cycles() != 0u) {
        return fail("machine shutdown did not unbind/reset its runtime safely");
    }
    if (machine.end_cpu_slice(NDS_ARM9) != NdsMachineStatus::NotInitialized) {
        return fail("shutdown machine accepted a new CPU slice");
    }

    oracis_host_set_keypad_state(0u);
    NdsMachine successor(SharedWramMode::Arm9All32K);
    if (successor.initialize() != NdsMachineStatus::Ok ||
        successor.shutdown() != NdsMachineStatus::Ok) {
        return fail("machine ownership was not released after shutdown");
    }

    std::puts("NdsMachine ownership, MMIO binding and slice clock tests passed");
    return 0;
}
