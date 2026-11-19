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

    /* NDS7 HALT wakes on IE&IF even with IME=0 and CPSR.I=1. */
    machine.cpu_state(NDS_ARM7)->cpsr |= CPSR_I_BIT;
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IeAddress, 4u, 1u);
    if (machine.write_arm7_halt_control(0x80u) != NdsMachineStatus::Ok ||
        !machine.cpu_is_halted(NDS_ARM7) ||
        machine.cpu_can_execute(NDS_ARM7) ||
        machine.begin_cpu_slice(NDS_ARM7, 1u) != NdsMachineStatus::CpuHalted) {
        return fail("ARM7 HALT was not held while IE&IF was clear");
    }
    machine.interrupt_control().request_irq(NDS_ARM7, 1u);
    if (!machine.cpu_can_execute(NDS_ARM7) ||
        machine.cpu_is_halted(NDS_ARM7)) {
        return fail("ARM7 HALT did not wake on IE&IF with IME disabled");
    }

    /* An interrupt already pending at the ARM7 HALT boundary prevents sleep. */
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().request_irq(NDS_ARM7, 1u);
    if (machine.write_arm7_halt_control(0x80u) != NdsMachineStatus::Ok ||
        machine.cpu_is_halted(NDS_ARM7)) {
        return fail("ARM7 HALT ignored an already pending enabled interrupt");
    }

    /* ARM9 Wait-For-Interrupt uses the IME-gated IRQ line, not CPSR.I. */
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IeAddress, 4u, 2u);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::ImeAddress, 4u, 0u);
    machine.interrupt_control().request_irq(NDS_ARM9, 2u);
    if (machine.enter_cpu_halt(NDS_ARM9) != NdsMachineStatus::Ok ||
        !machine.cpu_is_halted(NDS_ARM9) ||
        machine.cpu_can_execute(NDS_ARM9)) {
        return fail("ARM9 WFI woke while IME kept the IRQ line disabled");
    }
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::ImeAddress, 4u, 1u);
    machine.cpu_state(NDS_ARM9)->cpsr |= CPSR_I_BIT;
    if (!machine.cpu_can_execute(NDS_ARM9) ||
        machine.cpu_is_halted(NDS_ARM9) ||
        (machine.cpu_state(NDS_ARM9)->cpsr & CPSR_I_BIT) == 0u) {
        return fail("ARM9 WFI did not wake on IME/IE/IF independently of CPSR.I");
    }

    if (machine.write_arm7_halt_control(0x40u) !=
            NdsMachineStatus::UnsupportedPowerMode ||
        machine.write_arm7_halt_control(0xC0u) !=
            NdsMachineStatus::UnsupportedPowerMode ||
        machine.cpu_is_halted(NDS_ARM7)) {
        return fail("unsupported GBA-mode/SLEEP requests changed CPU run state");
    }

    /* NDS BIOS SWI 06 dispatches HALT through the currently bound machine. */
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    if (machine.begin_cpu_slice(NDS_ARM7, 5u) != NdsMachineStatus::Ok) {
        return fail("ARM7 HALT SWI test slice did not begin");
    }
    runtime_swi(0x06u);
    const bool halt_swi_yielded = runtime_should_yield();
    const bool halt_swi_halted = machine.cpu_is_halted(NDS_ARM7);
    const uint32_t halt_swi_pending =
        machine.interrupt_control().pending_enabled(NDS_ARM7);
    const uint32_t halt_swi_if =
        machine.interrupt_control().read(
            NDS_ARM7, NdsInterruptControl::IfAddress, 4u).value;
    const auto halt_swi_end = machine.end_cpu_slice(NDS_ARM7);
    if (!halt_swi_yielded || !halt_swi_halted ||
        halt_swi_end != NdsMachineStatus::Ok) {
        std::fprintf(stderr,
            "HALT SWI diagnostic: yield=%d halted=%d IE&IF=0x%08X IF=0x%08X "
            "active_cpu=%u end_status=%u\n",
            halt_swi_yielded ? 1 : 0,
            halt_swi_halted ? 1 : 0,
            halt_swi_pending,
            halt_swi_if,
            static_cast<unsigned>(g_nds_active),
            static_cast<unsigned>(halt_swi_end));
        return fail("BIOS HALT SWI did not set ARM7 HALT and yield its slice");
    }
    machine.interrupt_control().request_irq(NDS_ARM7, 1u);
    if (!machine.cpu_can_execute(NDS_ARM7)) {
        return fail("ARM7 did not resume after the SWI HALT test interrupt");
    }

    /* ARM7 CustomHalt SWI writes its byte argument from R2 to HALTCNT. */
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    if (machine.begin_cpu_slice(NDS_ARM7, 5u) != NdsMachineStatus::Ok) {
        return fail("ARM7 CustomHalt test slice did not begin");
    }
    g_cpu.R[0] = 0x40u; /* Must be ignored by CustomHalt. */
    g_cpu.R[2] = 0x80u;
    runtime_swi(0x1Fu);
    if (!runtime_should_yield() || !machine.cpu_is_halted(NDS_ARM7) ||
        machine.end_cpu_slice(NDS_ARM7) != NdsMachineStatus::Ok) {
        return fail("ARM7 CustomHalt SWI did not write HALTCNT and yield");
    }
    machine.interrupt_control().request_irq(NDS_ARM7, 1u);
    if (!machine.cpu_is_halted(NDS_ARM7)) {
        return fail("ARM7 CustomHalt left HALT before the IRQ boundary check");
    }

    /* ARM9 CP15 WFI is wired to the same machine state transition. */
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::ImeAddress, 4u, 1u);
    if (machine.begin_cpu_slice(NDS_ARM9, 5u) != NdsMachineStatus::Ok) {
        return fail("ARM9 CP15 WFI test slice did not begin");
    }
    runtime_coproc_write(15u, 0u, 7u, 0u, 4u, 0u);
    if (!runtime_should_yield() || !machine.cpu_is_halted(NDS_ARM9) ||
        machine.end_cpu_slice(NDS_ARM9) != NdsMachineStatus::Ok) {
        return fail("ARM9 CP15 WFI did not set HALT and yield its slice");
    }
    machine.interrupt_control().request_irq(NDS_ARM9, 2u);
    if (!machine.cpu_can_execute(NDS_ARM9)) {
        return fail("ARM9 CP15 WFI did not resume after its IRQ line asserted");
    }

    /*
     * IRQ delivery is deliberately performed only after both CPU slices have
     * ended. The helper must preserve pending IF and use the caller's exact
     * architectural LR/vector base rather than deriving either from R15.
     */
    constexpr uint32_t machine_irq = UINT32_C(1);
    auto *arm9 = machine.cpu_state(NDS_ARM9);
    auto *arm7 = machine.cpu_state(NDS_ARM7);
    if (arm9 == nullptr || arm7 == nullptr) {
        return fail("machine did not expose both CPU states for IRQ delivery");
    }

    /* ARM9: IME and CPSR.I independently gate exception entry. */
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IeAddress, 4u, machine_irq);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::ImeAddress, 4u, 0u);
    machine.interrupt_control().request_irq(NDS_ARM9, machine_irq);
    arm9->cpsr = ORACIS_ARM_MODE_SYSTEM | CPSR_T_BIT | CPSR_Z_BIT;
    arm9->R[15] = UINT32_C(0x02004000);
    const uint32_t arm9_original_cpsr = arm9->cpsr;
    if (machine.try_deliver_pending_irq(
            NDS_ARM9, UINT32_C(0x02004006), UINT32_C(0xFFFF0000)) !=
            NdsMachineStatus::IrqNotEligible ||
        arm9->cpsr != arm9_original_cpsr ||
        arm9->R[15] != UINT32_C(0x02004000)) {
        return fail("ARM9 entered IRQ while IME was disabled or mutated state");
    }

    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::ImeAddress, 4u, 1u);
    arm9->cpsr |= CPSR_I_BIT;
    const uint32_t arm9_masked_cpsr = arm9->cpsr;
    if (machine.try_deliver_pending_irq(
            NDS_ARM9, UINT32_C(0x02004006), UINT32_C(0xFFFF0000)) !=
            NdsMachineStatus::IrqNotEligible ||
        arm9->cpsr != arm9_masked_cpsr) {
        return fail("ARM9 entered IRQ while CPSR.I was set");
    }

    arm9->cpsr &= ~CPSR_I_BIT;
    if (machine.begin_cpu_slice(NDS_ARM9, 1u) != NdsMachineStatus::Ok ||
        machine.try_deliver_pending_irq(
            NDS_ARM9, UINT32_C(0x02004006), UINT32_C(0xFFFF0000)) !=
            NdsMachineStatus::SliceAlreadyActive ||
        machine.end_cpu_slice(NDS_ARM9) != NdsMachineStatus::Ok) {
        return fail("IRQ delivery was not blocked during an active CPU slice");
    }

    const uint32_t arm9_irq_cpsr = arm9->cpsr;
    if (machine.try_deliver_pending_irq(
            NDS_ARM9, UINT32_C(0x02004006), UINT32_C(0xFFFF0000)) !=
            NdsMachineStatus::IrqEntered ||
        (arm9->cpsr & ORACIS_ARM_MODE_MASK) != ORACIS_ARM_MODE_IRQ ||
        (arm9->cpsr & CPSR_T_BIT) != 0u ||
        (arm9->cpsr & CPSR_I_BIT) == 0u ||
        arm9->banked_spsr[ARM_BANK_IRQ] != arm9_irq_cpsr ||
        arm9->R[14] != UINT32_C(0x02004006) ||
        arm9->R[15] != UINT32_C(0xFFFF0018) ||
        (machine.interrupt_control().read(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value &
            machine_irq) == 0u ||
        g_nds_active != NDS_ARM9) {
        return fail("ARM9 IRQ entry did not save context/use supplied LR and vector");
    }

    /* ARM7 wakes from HALT on IE&IF, but CPSR.I still blocks entry. */
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IeAddress, 4u, machine_irq);
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::ImeAddress, 4u, 1u);
    machine.interrupt_control().request_irq(NDS_ARM7, machine_irq);
    arm7->cpsr = ORACIS_ARM_MODE_SYSTEM | CPSR_T_BIT | CPSR_Z_BIT | CPSR_I_BIT;
    arm7->R[15] = UINT32_C(0x02005000);
    const uint32_t arm7_masked_cpsr = arm7->cpsr;
    if (machine.try_deliver_pending_irq(
            NDS_ARM7, UINT32_C(0x02005002), 0u) !=
            NdsMachineStatus::IrqNotEligible ||
        machine.cpu_is_halted(NDS_ARM7) ||
        arm7->cpsr != arm7_masked_cpsr) {
        return fail("ARM7 did not wake on IE&IF while keeping CPSR.I entry mask");
    }

    arm7->cpsr &= ~CPSR_I_BIT;
    const uint32_t arm7_irq_cpsr = arm7->cpsr;
    if (machine.try_deliver_pending_irq(
            NDS_ARM7, UINT32_C(0x02005002), 0u) !=
            NdsMachineStatus::IrqEntered ||
        (arm7->cpsr & ORACIS_ARM_MODE_MASK) != ORACIS_ARM_MODE_IRQ ||
        (arm7->cpsr & CPSR_T_BIT) != 0u ||
        (arm7->cpsr & CPSR_I_BIT) == 0u ||
        arm7->banked_spsr[ARM_BANK_IRQ] != arm7_irq_cpsr ||
        arm7->R[14] != UINT32_C(0x02005002) ||
        arm7->R[15] != UINT32_C(0x00000018) ||
        (machine.interrupt_control().read(
            NDS_ARM7, NdsInterruptControl::IfAddress, 4u).value &
            machine_irq) == 0u ||
        g_nds_active != NDS_ARM7) {
        return fail("ARM7 IRQ entry did not wake/enter with supplied LR and vector");
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

    std::puts("NdsMachine ownership, MMIO binding, slice clock and HALT tests passed");
    return 0;
}
