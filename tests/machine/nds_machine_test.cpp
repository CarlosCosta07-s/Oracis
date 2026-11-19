#include "oracis/machine/nds_machine.hpp"

#include "oracis/memory/guest_memory_abi.h"
#include "oracis/recomp/runtime_arm.h"
#include "oracis/recomp/runtime_scheduler.h"
#include "oracis/recomp/static_dispatch_abi.h"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_machine_test: %s\n", message);

    return 1;
}

uint32_t g_static_dispatch_calls[2]{};

void static_arm9_test_bank()
{
    ++g_static_dispatch_calls[0];
    runtime_tick(1u);
}

void static_arm7_test_bank()
{
    ++g_static_dispatch_calls[1];
    runtime_tick(1u);
}

struct SchedulerFixture {
    uint32_t calls[2]{};
    uint64_t budgets[2]{};
    bool yield_early_arm9 = false;
    uint32_t irq_context_calls[2]{};
};

bool execute_test_slice(
    void *opaque, NdsCpu cpu, ArmCpuState *state, uint64_t cycle_budget)
{
    auto *fixture = static_cast<SchedulerFixture *>(opaque);
    if (fixture == nullptr || state == nullptr ||
        (cpu != NDS_ARM9 && cpu != NDS_ARM7) ||
        cycle_budget > UINT32_MAX) {
        return false;
    }

    const unsigned index = static_cast<unsigned>(cpu);
    ++fixture->calls[index];
    fixture->budgets[index] = cycle_budget;

    if (cpu == NDS_ARM9 && fixture->yield_early_arm9 &&
        cycle_budget > 1u) {
        runtime_tick(static_cast<uint32_t>(cycle_budget / 2u));
        return oracis_scheduler_request_yield(cpu) != 0 &&
               runtime_should_yield();
    }

    runtime_tick(static_cast<uint32_t>(cycle_budget));
    return runtime_should_yield();
}

bool provide_test_irq_context(
    void *opaque, NdsCpu cpu, const ArmCpuState *state,
    uint32_t *return_address, uint32_t *vector_base)
{
    auto *fixture = static_cast<SchedulerFixture *>(opaque);
    if (fixture == nullptr || state == nullptr ||
        return_address == nullptr || vector_base == nullptr ||
        (cpu != NDS_ARM9 && cpu != NDS_ARM7)) {
        return false;
    }

    ++fixture->irq_context_calls[static_cast<unsigned>(cpu)];
    *return_address = cpu == NDS_ARM7
        ? UINT32_C(0x02005002)
        : UINT32_C(0x02004006);
    *vector_base = 0u;
    return true;
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
        machine.cpu_state(NDS_ARM7)->cpsr != ORACIS_ARM_MODE_SYSTEM ||
        machine.exception_vector_base(NDS_ARM9) != UINT32_C(0xFFFF0000) ||
        runtime_coproc_read(15u, 0u, 1u, 0u, 0u) !=
            oracis::memory::NdsArm9Tcm::ResetControl ||
        runtime_coproc_read(15u, 0u, 0u, 0u, 0u) != UINT32_C(0x41059461) ||
        runtime_coproc_read(15u, 0u, 0u, 0u, 2u) != UINT32_C(0x00140180) ||
        machine.exception_vector_base(NDS_ARM7) != 0u) {
        return fail("machine did not bind CPU states or apply NDS reset vector bases");
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

    /*
     * Configure ARM9 TCM through CP15. The 32KB ITCM is mirrored through a
     * 32MB low-address aperture; the 16KB DTCM overlays main-RAM mirror 1.
     */
    constexpr uint32_t tcm_control = UINT32_C(0x00052078);
    constexpr uint32_t dtcm_disabled_control = UINT32_C(0x00042078);
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 could not be selected for TCM configuration");
    }
    runtime_coproc_write(15u, 0u, 9u, 1u, 1u, UINT32_C(0x00000020));
    runtime_coproc_write(15u, 0u, 9u, 1u, 0u, UINT32_C(0x027C000A));
    runtime_coproc_write(15u, 0u, 1u, 0u, 0u, tcm_control);
    if (runtime_coproc_read(15u, 0u, 9u, 1u, 1u) != UINT32_C(0x20) ||
        runtime_coproc_read(15u, 0u, 9u, 1u, 0u) != UINT32_C(0x027C000A) ||
        runtime_coproc_read(15u, 0u, 1u, 0u, 0u) != tcm_control) {
        return fail("CP15 did not preserve the supported ARM9 TCM registers");
    }

    if (machine.guest_memory().write(
            NDS_ARM9, UINT32_C(0x023C0000), 4u,
            UINT32_C(0x11223344)) != oracis::memory::Status::Ok ||
        machine.guest_memory().write(
            NDS_ARM9, UINT32_C(0x027C0000), 4u,
            UINT32_C(0xAABBCCDD)) != oracis::memory::Status::Ok ||
        machine.guest_memory().read(
            NDS_ARM9, UINT32_C(0x027C0000), 4u).value !=
            UINT32_C(0xAABBCCDD) ||
        machine.guest_memory().read(
            NDS_ARM9, UINT32_C(0x023C0000), 4u).value !=
            UINT32_C(0x11223344)) {
        return fail("DTCM overlay did not preserve the underlying main-RAM mirror");
    }

    if (machine.guest_memory().write(
            NDS_ARM9, UINT32_C(0x00000020), 4u,
            UINT32_C(0x55667788)) != oracis::memory::Status::Ok ||
        machine.guest_memory().read(
            NDS_ARM9, UINT32_C(0x00008020), 4u).value !=
            UINT32_C(0x55667788) ||
        machine.guest_memory().read(
            NDS_ARM7, UINT32_C(0x00000020), 4u).status !=
            oracis::memory::Status::Unmapped) {
        return fail("ITCM physical storage did not mirror privately to ARM9");
    }

    runtime_coproc_write(15u, 0u, 1u, 0u, 0u, dtcm_disabled_control);
    if (machine.guest_memory().read(
            NDS_ARM9, UINT32_C(0x027C0000), 4u).value !=
            UINT32_C(0x11223344)) {
        return fail("disabling DTCM did not reveal the underlying RAM mirror");
    }
    runtime_coproc_write(15u, 0u, 1u, 0u, 0u, tcm_control);
    if (machine.guest_memory().read(
            NDS_ARM9, UINT32_C(0x027C0000), 4u).value !=
            UINT32_C(0xAABBCCDD)) {
        return fail("re-enabling DTCM did not restore its physical backing");
    }

    /* Load pre-extracted segments only after both CPU destinations pass preflight. */
    const uint8_t arm9_program[] = {0x00u, 0x00u, 0xA0u, 0xE1u};
    const uint8_t arm7_program[] = {0x70u, 0x47u};
    const oracis::machine::NdsProgramSegment arm9_image = {
        NDS_ARM9, UINT32_C(0x02006000),
        static_cast<uint32_t>(sizeof(arm9_program)),
        UINT32_C(0x02006000), false, arm9_program
    };
    const oracis::machine::NdsProgramSegment invalid_arm7_image = {
        NDS_ARM7, UINT32_C(0x05000000),
        static_cast<uint32_t>(sizeof(arm7_program)),
        UINT32_C(0x05000000), true, arm7_program
    };
    if (machine.load_program_segments(
            arm9_image, invalid_arm7_image) !=
            NdsMachineStatus::ProgramMemoryUnavailable ||
        machine.program_loaded() ||
        machine.guest_memory().validate_identity(
            NDS_ARM9, arm9_image.load_address, arm9_image.size,
            arm9_program) != oracis::memory::Status::MissingWriteProvenance) {
        return fail("invalid ARM7 destination partially installed ARM9 program bytes");
    }

    const oracis::machine::NdsProgramSegment arm7_image = {
        NDS_ARM7, UINT32_C(0x02381000),
        static_cast<uint32_t>(sizeof(arm7_program)),
        UINT32_C(0x02381000), true, arm7_program
    };
    if (machine.load_program_segments(arm9_image, arm7_image) !=
            NdsMachineStatus::Ok ||
        !machine.program_loaded() ||
        machine.cpu_state(NDS_ARM9)->R[15] != arm9_image.entry_pc ||
        machine.cpu_state(NDS_ARM7)->R[15] != arm7_image.entry_pc ||
        (machine.cpu_state(NDS_ARM9)->cpsr & CPSR_T_BIT) != 0u ||
        (machine.cpu_state(NDS_ARM7)->cpsr & CPSR_T_BIT) == 0u ||
        machine.guest_memory().validate_identity(
            NDS_ARM9, arm9_image.load_address, arm9_image.size,
            arm9_program) != oracis::memory::Status::Ok ||
        machine.guest_memory().validate_identity(
            NDS_ARM7, arm7_image.load_address, arm7_image.size,
            arm7_program) != oracis::memory::Status::Ok) {
        return fail("valid ARM9/ARM7 segments were not loaded with correct entry modes");
    }
    if (machine.load_program_segments(arm9_image, arm7_image) !=
        NdsMachineStatus::ProgramAlreadyLoaded) {
        return fail("machine accepted a second program load without reset");
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

    /*
     * The dual-core round advances both CPUs toward one shared bus-time
     * watermark, preserving the ARM9:ARM7 2:1 cycle ratio.
     */
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IeAddress, 4u, 0u);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::ImeAddress, 4u, 0u);
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IeAddress, 4u, 0u);
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::ImeAddress, 4u, 0u);
    arm9->cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7->cpsr = ORACIS_ARM_MODE_SYSTEM;
    SchedulerFixture scheduler_fixture{};
    scheduler_fixture.yield_early_arm9 = true;

    if (machine.run_dual_core_round(
            0u, &execute_test_slice, &provide_test_irq_context,
            &scheduler_fixture) != NdsMachineStatus::InvalidCycleBudget ||
        machine.run_dual_core_round(
            1u, nullptr, &provide_test_irq_context,
            &scheduler_fixture) != NdsMachineStatus::SliceExecutionFailed) {
        return fail("dual-core scheduler accepted an invalid round configuration");
    }

    if (machine.run_dual_core_round(
            1u, &execute_test_slice, &provide_test_irq_context,
            &scheduler_fixture) != NdsMachineStatus::Ok ||
        scheduler_fixture.calls[0] != 1u ||
        scheduler_fixture.calls[1] != 1u ||
        scheduler_fixture.budgets[0] != 2u ||
        scheduler_fixture.budgets[1] != 1u ||
        machine.global_bus_cycles() != 3u ||
        machine.system_clock().cpu_cycle_count(NDS_ARM9) != 6u ||
        machine.system_clock().cpu_cycle_count(NDS_ARM7) != 3u ||
        oracis_scheduler_cycle_count(NDS_ARM9) != 3u) {
        return fail("dual-core scheduler failed 2:1 timing or early-yield accounting");
    }

    /* A halted ARM7 advances only its elapsed clock time; it is not dispatched. */
    if (machine.write_arm7_halt_control(0x80u) != NdsMachineStatus::Ok ||
        !machine.cpu_is_halted(NDS_ARM7)) {
        return fail("ARM7 did not enter HALT before the scheduler idle test");
    }
    const uint32_t arm7_calls_before_halt = scheduler_fixture.calls[1];
    if (machine.run_dual_core_round(
            1u, &execute_test_slice, &provide_test_irq_context,
            &scheduler_fixture) != NdsMachineStatus::Ok ||
        scheduler_fixture.calls[1] != arm7_calls_before_halt ||
        !machine.cpu_is_halted(NDS_ARM7) ||
        machine.global_bus_cycles() != 4u ||
        machine.system_clock().cpu_cycle_count(NDS_ARM7) != 4u) {
        return fail("dual-core scheduler executed HALTed ARM7 or failed to advance its clock");
    }

    /* A pending enabled ARM7 interrupt wakes HALT and is delivered before code runs. */
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IeAddress, 4u, machine_irq);
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::ImeAddress, 4u, 1u);
    machine.interrupt_control().request_irq(NDS_ARM7, machine_irq);
    arm7->cpsr = ORACIS_ARM_MODE_SYSTEM | CPSR_Z_BIT;
    const uint32_t arm7_round_irq_cpsr = arm7->cpsr;
    if (machine.run_dual_core_round(
            1u, &execute_test_slice, &provide_test_irq_context,
            &scheduler_fixture) != NdsMachineStatus::Ok ||
        machine.cpu_is_halted(NDS_ARM7) ||
        scheduler_fixture.irq_context_calls[1] != 1u ||
        (arm7->cpsr & ORACIS_ARM_MODE_MASK) != ORACIS_ARM_MODE_IRQ ||
        arm7->banked_spsr[ARM_BANK_IRQ] != arm7_round_irq_cpsr ||
        arm7->R[14] != UINT32_C(0x02005002) ||
        arm7->R[15] != UINT32_C(0x00000018) ||
        scheduler_fixture.calls[1] != arm7_calls_before_halt + 1u ||
        machine.global_bus_cycles() != 5u) {
        return fail("dual-core scheduler did not wake and service ARM7 IRQ at its boundary");
    }

    /*
     * Exercise the production adapter that enters generated banks through
     * runtime_dispatch, rather than the test-only slice callback above.
     */
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IeAddress, 4u, 0u);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::ImeAddress, 4u, 0u);
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IeAddress, 4u, 0u);
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::ImeAddress, 4u, 0u);
    arm9->cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7->cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm9->R[15] = UINT32_C(0x02006000);
    arm7->R[15] = UINT32_C(0x02381000);
    g_static_dispatch_calls[0] = 0u;
    g_static_dispatch_calls[1] = 0u;

    const NdsDispatchEntry arm9_test_bank[] = {
        {UINT32_C(0x02006000), 0u, &static_arm9_test_bank, nullptr}
    };
    const NdsDispatchEntry arm7_test_bank[] = {
        {UINT32_C(0x02381000), 0u, &static_arm7_test_bank, nullptr}
    };
    if (oracis_static_dispatch_register_bank(
            NDS_ARM9, arm9_test_bank, 1u, "machine-test-arm9") != 0 ||
        oracis_static_dispatch_register_bank(
            NDS_ARM7, arm7_test_bank, 1u, "machine-test-arm7") != 0) {
        oracis_static_dispatch_clear();
        return fail("machine test static banks could not be registered");
    }

    const NdsMachineStatus static_round = machine.run_dual_core_round(
        1u, &NdsMachine::execute_registered_bank_slice,
        &provide_test_irq_context, &scheduler_fixture);
    const bool banks_dispatched =
        static_round == NdsMachineStatus::Ok &&
        g_static_dispatch_calls[0] == 2u &&
        g_static_dispatch_calls[1] == 1u &&
        machine.global_bus_cycles() == 6u;

    const bool arm9_unregistered = oracis_static_dispatch_unregister_bank(
        NDS_ARM9, arm9_test_bank, 1u) != 0;
    const bool arm7_unregistered = oracis_static_dispatch_unregister_bank(
        NDS_ARM7, arm7_test_bank, 1u) != 0;
    if (!banks_dispatched || !arm9_unregistered || !arm7_unregistered) {
        oracis_static_dispatch_clear();
        return fail("dual-core rounds did not invoke both registered static banks");
    }

    /*
     * Generated-bank validation must consult the machine's live GuestMemory.
     * Test both instruction bytes and the dependent immutable data closure.
     */
    constexpr uint32_t validated_arm9_pc = UINT32_C(0x02007000);
    constexpr uint32_t validated_arm9_data = UINT32_C(0x02007100);
    constexpr uint32_t validated_arm7_pc = UINT32_C(0x02382000);
    const uint8_t validated_arm9_code[] = {0x00u, 0x00u, 0xA0u, 0xE1u};
    const uint8_t validated_arm9_data_bytes[] = {
        0x11u, 0x22u, 0x33u, 0x44u
    };
    const uint8_t validated_arm7_code[] = {0x70u, 0x47u};
    if (machine.guest_memory().write_bytes(
            NDS_ARM9, validated_arm9_pc, validated_arm9_code,
            static_cast<uint32_t>(sizeof(validated_arm9_code))) !=
            oracis::memory::Status::Ok ||
        machine.guest_memory().write_bytes(
            NDS_ARM9, validated_arm9_data, validated_arm9_data_bytes,
            static_cast<uint32_t>(sizeof(validated_arm9_data_bytes))) !=
            oracis::memory::Status::Ok ||
        machine.guest_memory().write_bytes(
            NDS_ARM7, validated_arm7_pc, validated_arm7_code,
            static_cast<uint32_t>(sizeof(validated_arm7_code))) !=
            oracis::memory::Status::Ok) {
        return fail("validated static-bank fixture bytes could not be materialized");
    }

    const NdsStaticValidationRange arm9_dependency[] = {{
        validated_arm9_data,
        static_cast<uint32_t>(sizeof(validated_arm9_data_bytes)),
        validated_arm9_data_bytes
    }};
    const NdsStaticValidation arm9_validation = {
        validated_arm9_pc,
        static_cast<uint32_t>(sizeof(validated_arm9_code)),
        validated_arm9_code,
        arm9_dependency,
        1u
    };
    const NdsStaticValidation arm7_validation = {
        validated_arm7_pc,
        static_cast<uint32_t>(sizeof(validated_arm7_code)),
        validated_arm7_code,
        nullptr,
        0u
    };
    const NdsDispatchEntry validated_arm9_bank[] = {{
        validated_arm9_pc, 0u, &static_arm9_test_bank, &arm9_validation
    }};
    const NdsDispatchEntry validated_arm7_bank[] = {{
        validated_arm7_pc, 1u, &static_arm7_test_bank, &arm7_validation
    }};
    if (oracis_static_dispatch_register_bank(
            NDS_ARM9, validated_arm9_bank, 1u,
            "machine-validated-arm9") != ORACIS_DISPATCH_REGISTER_OK ||
        oracis_static_dispatch_register_bank(
            NDS_ARM7, validated_arm7_bank, 1u,
            "machine-validated-arm7") != ORACIS_DISPATCH_REGISTER_OK) {
        oracis_static_dispatch_clear();
        return fail("validated static banks could not be registered");
    }

    uint32_t validation_status = UINT32_MAX;
    uint32_t validation_candidates = 0u;
    const NdsDispatchEntry *inactive_candidate = nullptr;
    arm9->cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        oracis_static_dispatch_lookup(
            NDS_ARM9, validated_arm9_pc, arm9->cpsr,
            &validation_status, &validation_candidates,
            &inactive_candidate) != validated_arm9_bank ||
        validation_status != ORACIS_DISPATCH_LOOKUP_FOUND ||
        validation_candidates != 1u || inactive_candidate != nullptr) {
        return fail("live ARM9 code and dependency bytes did not validate");
    }

    /* The registry CPU must match the runtime-selected CPU before validation. */
    arm7->cpsr = ORACIS_ARM_MODE_SYSTEM | CPSR_T_BIT;
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7) ||
        oracis_static_dispatch_lookup(
            NDS_ARM9, validated_arm9_pc, arm9->cpsr,
            &validation_status, &validation_candidates,
            &inactive_candidate) != nullptr ||
        validation_status != ORACIS_DISPATCH_LOOKUP_MISS ||
        inactive_candidate != validated_arm9_bank) {
        return fail("ARM9 bank validation was allowed while ARM7 was selected");
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 runtime state could not be reselected after CPU mismatch test");
    }

    const uint8_t altered_dependency[] = {
        0x11u, 0x22u, 0x99u, 0x44u
    };
    if (machine.guest_memory().write_bytes(
            NDS_ARM9, validated_arm9_data, altered_dependency,
            static_cast<uint32_t>(sizeof(altered_dependency))) !=
            oracis::memory::Status::Ok ||
        oracis_static_dispatch_lookup(
            NDS_ARM9, validated_arm9_pc, arm9->cpsr,
            &validation_status, &validation_candidates,
            &inactive_candidate) != nullptr ||
        validation_status != ORACIS_DISPATCH_LOOKUP_MISS ||
        inactive_candidate != validated_arm9_bank) {
        return fail("changed ARM9 dependency bytes were accepted by dispatch");
    }

    if (machine.guest_memory().write_bytes(
            NDS_ARM9, validated_arm9_data, validated_arm9_data_bytes,
            static_cast<uint32_t>(sizeof(validated_arm9_data_bytes))) !=
            oracis::memory::Status::Ok) {
        return fail("ARM9 dependency fixture could not be restored");
    }
    const uint8_t altered_arm9_code[] = {0x00u, 0x00u, 0xA1u, 0xE1u};
    if (machine.guest_memory().write_bytes(
            NDS_ARM9, validated_arm9_pc, altered_arm9_code,
            static_cast<uint32_t>(sizeof(altered_arm9_code))) !=
            oracis::memory::Status::Ok ||
        oracis_static_dispatch_lookup(
            NDS_ARM9, validated_arm9_pc, arm9->cpsr,
            &validation_status, &validation_candidates,
            &inactive_candidate) != nullptr ||
        validation_status != ORACIS_DISPATCH_LOOKUP_MISS ||
        inactive_candidate != validated_arm9_bank) {
        return fail("changed ARM9 instruction bytes were accepted by dispatch");
    }

    arm7->cpsr = ORACIS_ARM_MODE_SYSTEM | CPSR_T_BIT;
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        oracis_static_dispatch_lookup(
            NDS_ARM7, validated_arm7_pc | 1u, arm7->cpsr,
            &validation_status, &validation_candidates,
            &inactive_candidate) != nullptr ||
        validation_status != ORACIS_DISPATCH_LOOKUP_MISS ||
        inactive_candidate != validated_arm7_bank) {
        return fail("ARM7 validation used the wrong active CPU memory context");
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7) ||
        oracis_static_dispatch_lookup(
            NDS_ARM7, validated_arm7_pc | 1u, arm7->cpsr,
            &validation_status, &validation_candidates,
            &inactive_candidate) != validated_arm7_bank ||
        validation_status != ORACIS_DISPATCH_LOOKUP_FOUND ||
        inactive_candidate != nullptr) {
        return fail("live ARM7 Thumb code did not validate against ARM7 memory");
    }

    {
        NdsMachine competing_machine(SharedWramMode::Arm9All32K);
        if (competing_machine.initialize() !=
                NdsMachineStatus::RuntimeAlreadyBound) {
            return fail("a second machine was allowed to replace live CPU states");
        }
    }
    if (machine.global_bus_cycles() != 6u) {
        return fail("a rejected competing machine disturbed the active scheduler");
    }

    /*
     * ARM9 CP15 Control.V owns the high-vector selection. The legacy host
     * callback deliberately reports the low base; the machine must use CP15.
     */
    constexpr uint32_t cp15_vector_select =
        oracis::memory::NdsArm9Tcm::ResetControl;
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 could not be selected for unsupported CP15 write test");
    }
    uint32_t unsupported_cp15_control = 1u;
    if (oracis_arm_runtime_coprocessor_transfer(
            1u, 15u, 0u, 1u, 0u, 0u, &unsupported_cp15_control) != 0 ||
        machine.exception_vector_base(NDS_ARM9) != UINT32_C(0xFFFF0000)) {
        return fail("unsupported ARM9 CP15 MPU/cache control bits were silently accepted");
    }
    arm9->cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm9->R[15] = UINT32_C(0x02006000);
    arm7->cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 could not be selected for CP15 register access");
    }
    runtime_coproc_write(15u, 0u, 1u, 0u, 0u, cp15_vector_select);
    if (runtime_coproc_read(15u, 0u, 1u, 0u, 0u) != cp15_vector_select ||
        machine.exception_vector_base(NDS_ARM9) != UINT32_C(0xFFFF0000) ||
        machine.exception_vector_base(NDS_ARM7) != 0u) {
        return fail("CP15 Control.V did not select high ARM9 vectors independently");
    }

    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IeAddress, 4u, machine_irq);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::ImeAddress, 4u, 1u);
    machine.interrupt_control().request_irq(NDS_ARM9, machine_irq);
    SchedulerFixture cp15_irq_fixture{};
    if (machine.run_dual_core_round(
            1u, &execute_test_slice, &provide_test_irq_context,
            &cp15_irq_fixture) != NdsMachineStatus::Ok ||
        cp15_irq_fixture.irq_context_calls[0] != 1u ||
        (arm9->cpsr & ORACIS_ARM_MODE_MASK) != ORACIS_ARM_MODE_IRQ ||
        arm9->R[15] != UINT32_C(0xFFFF0018)) {
        return fail("dual-core IRQ scheduling ignored ARM9 CP15 high-vector selection");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 could not be reselected after high-vector IRQ test");
    }
    runtime_coproc_write(
        15u, 0u, 1u, 0u, 0u,
        oracis::memory::NdsArm9Tcm::RequiredControlOnesMask);
    arm9->cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm9->R[15] = UINT32_C(0x02006000);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::ImeAddress, 4u, 0u);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IeAddress, 4u, 0u);
    if (machine.exception_vector_base(NDS_ARM9) != 0u ||
        machine.global_bus_cycles() != 7u) {
        return fail("CP15 Control.V reset-to-low selection or cycle accounting failed");
    }

    /* Preserve the ARM7 Thumb mode expected by its registered dispatch entry. */
    arm7->cpsr = ORACIS_ARM_MODE_SYSTEM | CPSR_T_BIT;

    if (machine.shutdown() != NdsMachineStatus::Ok ||
        machine.initialized() ||
        oracis_arm_runtime_current_state() != nullptr ||
        oracis_bus_global_bus_cycles() != 0u) {
        return fail("machine shutdown did not unbind/reset its runtime safely");
    }
    if (machine.end_cpu_slice(NDS_ARM9) != NdsMachineStatus::NotInitialized) {
        return fail("shutdown machine accepted a new CPU slice");
    }

    /* The global validator must be cleared before this machine's memory dies. */
    validation_status = UINT32_MAX;
    inactive_candidate = nullptr;
    const NdsDispatchEntry *post_shutdown_arm9 = oracis_static_dispatch_lookup(
        NDS_ARM9, validated_arm9_pc, arm9->cpsr,
        &validation_status, &validation_candidates, &inactive_candidate);
    const uint32_t post_shutdown_arm9_status = validation_status;
    const uint32_t post_shutdown_arm9_candidates = validation_candidates;
    const NdsDispatchEntry *post_shutdown_arm9_inactive = inactive_candidate;

    const NdsDispatchEntry *post_shutdown_arm7 = oracis_static_dispatch_lookup(
        NDS_ARM7, validated_arm7_pc | 1u, arm7->cpsr,
        &validation_status, &validation_candidates, &inactive_candidate);
    const uint32_t post_shutdown_arm7_status = validation_status;
    const uint32_t post_shutdown_arm7_candidates = validation_candidates;
    const NdsDispatchEntry *post_shutdown_arm7_inactive = inactive_candidate;

    if (post_shutdown_arm9 != nullptr ||
        post_shutdown_arm9_status != ORACIS_DISPATCH_LOOKUP_MISS ||
        post_shutdown_arm9_candidates != 1u ||
        post_shutdown_arm9_inactive != validated_arm9_bank ||
        post_shutdown_arm7 != nullptr ||
        post_shutdown_arm7_status != ORACIS_DISPATCH_LOOKUP_MISS ||
        post_shutdown_arm7_candidates != 1u ||
        post_shutdown_arm7_inactive != validated_arm7_bank) {
        std::fprintf(stderr,
            "post-shutdown dispatch diagnostics: "
            "ARM9 selected=%p status=%u candidates=%u inactive=%p expected=%p; "
            "ARM7 selected=%p status=%u candidates=%u inactive=%p expected=%p; "
            "active_state=%p active_cpu=%u\n",
            static_cast<const void *>(post_shutdown_arm9),
            post_shutdown_arm9_status, post_shutdown_arm9_candidates,
            static_cast<const void *>(post_shutdown_arm9_inactive),
            static_cast<const void *>(validated_arm9_bank),
            static_cast<const void *>(post_shutdown_arm7),
            post_shutdown_arm7_status, post_shutdown_arm7_candidates,
            static_cast<const void *>(post_shutdown_arm7_inactive),
            static_cast<const void *>(validated_arm7_bank),
            static_cast<void *>(oracis_arm_runtime_current_state()),
            static_cast<unsigned>(g_nds_active));
        return fail("shutdown left the process-global GuestMemory validator installed");
    }
    if (oracis_static_dispatch_unregister_bank(
            NDS_ARM9, validated_arm9_bank, 1u) == 0 ||
        oracis_static_dispatch_unregister_bank(
            NDS_ARM7, validated_arm7_bank, 1u) == 0) {
        oracis_static_dispatch_clear();
        return fail("validated static banks could not be unregistered after shutdown");
    }

    oracis_host_set_keypad_state(0u);
    NdsMachine successor(SharedWramMode::Arm9All32K);
    if (successor.initialize() != NdsMachineStatus::Ok ||
        successor.shutdown() != NdsMachineStatus::Ok) {
        return fail("machine ownership was not released after shutdown");
    }

    std::puts("NdsMachine ownership, dual-core scheduling, dispatch and IRQ tests passed");
    return 0;
}
