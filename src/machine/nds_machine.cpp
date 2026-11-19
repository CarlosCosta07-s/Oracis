#include "oracis/machine/nds_machine.hpp"

#include "oracis/memory/guest_memory_abi.h"
#include "oracis/recomp/runtime_arm.h"
#include "oracis/recomp/runtime_scheduler.h"
#include "oracis/recomp/static_dispatch_abi.h"

#include <exception>
#include <limits>

namespace oracis::machine {

NdsMachine::NdsMachine(memory::SharedWramMode initial_wram_mode) noexcept
    : wram_control_(memory_, initial_wram_mode),
      interrupt_control_(memory_),
      timer_control_(memory_, interrupt_control_),
      keypad_control_(memory_, interrupt_control_),
      system_clock_(timer_control_)
{
}

NdsMachine::~NdsMachine()
{
    /*
     * A failed unbind would leave process-global ABI pointers referencing
     * members that are about to be destroyed. Terminate rather than permit a
     * silent use-after-free; callers should shut down at a safe machine stop.
     */
    if (shutdown() != NdsMachineStatus::Ok) {
        std::terminate();
    }
}

bool NdsMachine::valid_cpu(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 || cpu == NDS_ARM7;
}

unsigned NdsMachine::cpu_index(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 ? 0u : 1u;
}

int NdsMachine::runtime_power_handler(
    void *context, NdsCpu cpu, uint32_t operation, uint32_t value)
{
    auto *machine = static_cast<NdsMachine *>(context);
    if (machine == nullptr) return 0;

    if (operation == ORACIS_ARM_RUNTIME_POWER_HALT) {
        return machine->enter_cpu_halt(cpu) == NdsMachineStatus::Ok ? 1 : 0;
    }
    if (operation == ORACIS_ARM_RUNTIME_POWER_ARM7_HALTCNT &&
        cpu == NDS_ARM7) {
        return machine->write_arm7_halt_control(
            static_cast<uint8_t>(value)) == NdsMachineStatus::Ok ? 1 : 0;
    }
    return 0;
}

int NdsMachine::runtime_coprocessor_handler(
    void *context, NdsCpu cpu, uint32_t is_write,
    uint32_t cp_num, uint32_t op1, uint32_t crn,
    uint32_t crm, uint32_t op2, uint32_t *value)
{
    auto *machine = static_cast<NdsMachine *>(context);
    if (machine == nullptr || !machine->initialized_ ||
        cpu != NDS_ARM9 || is_write > 1u || value == nullptr ||
        cp_num != 15u || op1 != 0u || crn != 1u || crm != 0u || op2 != 0u) {
        return 0;
    }

    /* ARM946E-S CP15 c1 bit 13 selects 0x00000000 or 0xFFFF0000 vectors. */
    constexpr uint32_t kAlternateVectorSelect = UINT32_C(1) << 13u;
    if (is_write != 0u) {
        /* Refuse to claim support for MPU/cache/TCM bits not modeled yet. */
        if ((*value & ~kAlternateVectorSelect) != 0u) return 0;
        machine->arm9_cp15_control_ = *value;
    } else {
        *value = machine->arm9_cp15_control_ & kAlternateVectorSelect;
    }
    return 1;
}

int NdsMachine::runtime_validate_dispatch_bytes(
    NdsCpu cpu,
    const NdsStaticValidation *validation,
    uint32_t pc,
    uint32_t thumb,
    void *context)
{
    auto *machine = static_cast<NdsMachine *>(context);
    if (machine == nullptr || !machine->initialized_ ||
        !valid_cpu(cpu) || g_nds_active != cpu ||
        machine->cpu_state(cpu) != oracis_active_cpu_state ||
        validation == nullptr || validation->expected == nullptr ||
        validation->size == 0u || thumb > 1u) {
        return 0;
    }
    memory::GuestMemory *guest_memory = &machine->memory_;

    const uint64_t validation_begin = validation->addr;
    const uint64_t validation_end =
        validation_begin + static_cast<uint64_t>(validation->size);
    const uint64_t instruction_size = thumb != 0u ? 2u : 4u;
    const uint64_t instruction_end =
        static_cast<uint64_t>(pc) + instruction_size;
    if (pc < validation_begin || instruction_end > validation_end) {
        return 0;
    }

    if (guest_memory->validate_execute(
            cpu, validation->addr, validation->size) != memory::Status::Ok ||
        guest_memory->validate_identity(
            cpu, validation->addr, validation->size,
            validation->expected) != memory::Status::Ok) {
        return 0;
    }

    if (validation->dependency_count != 0u &&
        validation->dependencies == nullptr) {
        return 0;
    }
    for (uint32_t index = 0u;
         index < validation->dependency_count;
         ++index) {
        const NdsStaticValidationRange &dependency =
            validation->dependencies[index];
        if (dependency.expected == nullptr || dependency.size == 0u ||
            guest_memory->validate_identity(
                cpu, dependency.addr, dependency.size,
                dependency.expected) != memory::Status::Ok) {
            return 0;
        }
    }

    return 1;
}

NdsMachineStatus NdsMachine::initialize() noexcept
{
    if (initialized_ || wram_control_.installed()) {
        return NdsMachineStatus::AlreadyInitialized;
    }

    arm9_state_ = {};
    arm7_state_ = {};
    program_loaded_ = false;
    arm9_cp15_control_ = 0u;
    arm9_state_.cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7_state_.cpsr = ORACIS_ARM_MODE_SYSTEM;
    run_states_.fill(NdsCpuRunState::Running);

    if (!oracis_arm_runtime_bind_cpus(&arm9_state_, &arm7_state_)) {
        return NdsMachineStatus::RuntimeAlreadyBound;
    }
    runtime_bound_ = true;

    oracis_scheduler_reset(NDS_ARM9);
    oracis_scheduler_reset(NDS_ARM7);
    idle_core_cycles_offset_.fill(0u);
    next_round_first_cpu_ = NDS_ARM9;

    const auto rollback = [this](NdsMachineStatus cause) noexcept {
        return shutdown() == NdsMachineStatus::Ok
            ? cause : NdsMachineStatus::ShutdownFailed;
    };

    if (!oracis_arm_runtime_bind_power_handler(
            this, &NdsMachine::runtime_power_handler)) {
        return rollback(NdsMachineStatus::RuntimeAlreadyBound);
    }
    power_handler_bound_ = true;

    if (!oracis_arm_runtime_bind_coprocessor_handler(
            this, &NdsMachine::runtime_coprocessor_handler)) {
        return rollback(NdsMachineStatus::RuntimeAlreadyBound);
    }
    coprocessor_handler_bound_ = true;

    if (wram_control_.install() != memory::Status::Ok) {
        return rollback(NdsMachineStatus::MemoryMapFailed);
    }

    if (!oracis_bus_bind_memory(&memory_)) {
        return rollback(NdsMachineStatus::BusBindingFailed);
    }
    memory_bound_ = true;

    if (!oracis_bus_bind_wram_control(&wram_control_)) {
        return rollback(NdsMachineStatus::BusBindingFailed);
    }
    wram_bound_ = true;

    if (!oracis_bus_bind_interrupt_control(&interrupt_control_)) {
        return rollback(NdsMachineStatus::BusBindingFailed);
    }
    interrupt_bound_ = true;

    if (!oracis_bus_bind_timer_control(&timer_control_)) {
        return rollback(NdsMachineStatus::BusBindingFailed);
    }
    timer_bound_ = true;

    if (!oracis_bus_bind_keypad_control(&keypad_control_)) {
        return rollback(NdsMachineStatus::BusBindingFailed);
    }
    keypad_bound_ = true;

    if (!oracis_bus_bind_system_clock(&system_clock_)) {
        return rollback(NdsMachineStatus::BusBindingFailed);
    }
    clock_bound_ = true;

    oracis_static_dispatch_set_validation_callback(
        &NdsMachine::runtime_validate_dispatch_bytes, this);
    validation_bound_ = true;

    initialized_ = true;
    return NdsMachineStatus::Ok;
}

NdsMachineStatus NdsMachine::load_program_segments(
    const NdsProgramSegment &arm9,
    const NdsProgramSegment &arm7) noexcept
{
    if (!initialized_) return NdsMachineStatus::NotInitialized;
    if (program_loaded_) return NdsMachineStatus::ProgramAlreadyLoaded;
    if (slice_active_[0] || slice_active_[1]) {
        return NdsMachineStatus::SliceAlreadyActive;
    }

    if (system_clock_.global_bus_cycles() != 0u ||
        oracis_scheduler_cycle_count(NDS_ARM9) != 0u ||
        oracis_scheduler_cycle_count(NDS_ARM7) != 0u ||
        idle_core_cycles_offset_[0] != 0u ||
        idle_core_cycles_offset_[1] != 0u) {
        return NdsMachineStatus::ProgramLoadAfterExecution;
    }

    const auto valid_segment = [](const NdsProgramSegment &segment,
                                  NdsCpu expected_cpu) noexcept {
        if (segment.cpu != expected_cpu || segment.contents == nullptr ||
            segment.size == 0u) {
            return false;
        }

        const uint64_t load_begin = segment.load_address;
        const uint64_t load_end =
            load_begin + static_cast<uint64_t>(segment.size);
        const uint64_t entry_begin = segment.entry_pc;
        const uint64_t instruction_size =
            segment.entry_thumb ? UINT64_C(2) : UINT64_C(4);
        const uint64_t entry_end = entry_begin + instruction_size;
        const uint32_t alignment = segment.entry_thumb ? 2u : 4u;

        return load_end <= UINT64_C(0x1'0000'0000) &&
               segment.entry_pc % alignment == 0u &&
               entry_begin >= load_begin &&
               entry_end <= load_end;
    };

    if (!valid_segment(arm9, NDS_ARM9) ||
        !valid_segment(arm7, NDS_ARM7)) {
        return NdsMachineStatus::InvalidProgramImage;
    }

    /*
     * Preflight BOTH complete segments before writing either one. This makes
     * a missing, non-writable or non-executable ARM7 destination fail without
     * leaving ARM9 bytes installed.
     */
    const memory::Status arm9_write = memory_.validate_write(
        NDS_ARM9, arm9.load_address, arm9.size);
    const memory::Status arm9_execute = memory_.validate_execute(
        NDS_ARM9, arm9.load_address, arm9.size);
    const memory::Status arm7_write = memory_.validate_write(
        NDS_ARM7, arm7.load_address, arm7.size);
    const memory::Status arm7_execute = memory_.validate_execute(
        NDS_ARM7, arm7.load_address, arm7.size);
    if (arm9_write != memory::Status::Ok ||
        arm9_execute != memory::Status::Ok ||
        arm7_write != memory::Status::Ok ||
        arm7_execute != memory::Status::Ok) {
        return NdsMachineStatus::ProgramMemoryUnavailable;
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return NdsMachineStatus::SchedulerFailure;
    }

    if (memory_.write_bytes(
            NDS_ARM9, arm9.load_address, arm9.contents, arm9.size) !=
            memory::Status::Ok ||
        memory_.write_bytes(
            NDS_ARM7, arm7.load_address, arm7.contents, arm7.size) !=
            memory::Status::Ok) {
        return NdsMachineStatus::ProgramLoadFailed;
    }

    /* Confirm that the exact bytes used by the static banks are now live. */
    if (memory_.validate_identity(
            NDS_ARM9, arm9.load_address, arm9.size, arm9.contents) !=
            memory::Status::Ok ||
        memory_.validate_identity(
            NDS_ARM7, arm7.load_address, arm7.size, arm7.contents) !=
            memory::Status::Ok) {
        return NdsMachineStatus::ProgramLoadFailed;
    }

    arm9_state_ = {};
    arm7_state_ = {};
    arm9_state_.cpsr = ORACIS_ARM_MODE_SYSTEM |
        (arm9.entry_thumb ? CPSR_T_BIT : 0u);
    arm7_state_.cpsr = ORACIS_ARM_MODE_SYSTEM |
        (arm7.entry_thumb ? CPSR_T_BIT : 0u);
    arm9_state_.R[15] = arm9.entry_pc;
    arm7_state_.R[15] = arm7.entry_pc;
    run_states_.fill(NdsCpuRunState::Running);
    program_loaded_ = true;
    return NdsMachineStatus::Ok;
}

NdsMachineStatus NdsMachine::shutdown() noexcept
{
    const bool owns_runtime =
        runtime_bound_ || power_handler_bound_ ||
        coprocessor_handler_bound_ || memory_bound_ ||
        wram_bound_ || interrupt_bound_ || timer_bound_ ||
        keypad_bound_ || clock_bound_ || validation_bound_;
    if (!owns_runtime && !initialized_) {
        return NdsMachineStatus::Ok;
    }

    if (slice_active_[0]) {
        oracis_scheduler_end_slice(NDS_ARM9);
        slice_active_[0] = false;
    }
    if (slice_active_[1]) {
        oracis_scheduler_end_slice(NDS_ARM7);
        slice_active_[1] = false;
    }

    /* Do not leave a process-global callback pointing at this memory map. */
    if (validation_bound_) {
        oracis_static_dispatch_set_validation_callback(nullptr, nullptr);
        validation_bound_ = false;
    }

    if (clock_bound_) {
        if (!oracis_bus_unbind_system_clock(&system_clock_)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        clock_bound_ = false;
    }
    if (keypad_bound_) {
        if (!oracis_bus_unbind_keypad_control(&keypad_control_)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        keypad_bound_ = false;
    }
    if (timer_bound_) {
        if (!oracis_bus_unbind_timer_control(&timer_control_)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        timer_bound_ = false;
    }
    if (interrupt_bound_) {
        if (!oracis_bus_unbind_interrupt_control(&interrupt_control_)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        interrupt_bound_ = false;
    }
    if (wram_bound_) {
        if (!oracis_bus_unbind_wram_control(&wram_control_)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        wram_bound_ = false;
    }
    if (memory_bound_) {
        if (!oracis_bus_unbind_memory(&memory_)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        memory_bound_ = false;
    }
    if (coprocessor_handler_bound_) {
        if (!oracis_arm_runtime_unbind_coprocessor_handler(
                this, &NdsMachine::runtime_coprocessor_handler)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        coprocessor_handler_bound_ = false;
    }
    if (power_handler_bound_) {
        if (!oracis_arm_runtime_unbind_power_handler(
                this, &NdsMachine::runtime_power_handler)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        power_handler_bound_ = false;
    }
    if (runtime_bound_) {
        if (!oracis_arm_runtime_unbind_cpus(&arm9_state_, &arm7_state_)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        runtime_bound_ = false;
    }

    oracis_scheduler_reset(NDS_ARM9);
    oracis_scheduler_reset(NDS_ARM7);
    system_clock_.reset();
    run_states_.fill(NdsCpuRunState::Running);
    idle_core_cycles_offset_.fill(0u);
    next_round_first_cpu_ = NDS_ARM9;
    program_loaded_ = false;
    initialized_ = false;
    return NdsMachineStatus::Ok;
}

NdsMachineStatus NdsMachine::begin_cpu_slice(
    NdsCpu cpu, uint64_t cycle_budget) noexcept
{
    if (!initialized_) return NdsMachineStatus::NotInitialized;
    if (!valid_cpu(cpu)) return NdsMachineStatus::InvalidCpu;

    const unsigned index = cpu_index(cpu);
    if (!cpu_can_execute(cpu)) return NdsMachineStatus::CpuHalted;
    if (slice_active_[0] || slice_active_[1]) {
        return NdsMachineStatus::SliceAlreadyActive;
    }
    if (!oracis_arm_runtime_select_cpu(cpu) ||
        !oracis_scheduler_begin_slice(cpu, cycle_budget)) {
        return NdsMachineStatus::SchedulerFailure;
    }

    slice_active_[index] = true;
    return NdsMachineStatus::Ok;
}

NdsMachineStatus NdsMachine::end_cpu_slice(NdsCpu cpu) noexcept
{
    if (!initialized_) return NdsMachineStatus::NotInitialized;
    if (!valid_cpu(cpu)) return NdsMachineStatus::InvalidCpu;

    const unsigned index = cpu_index(cpu);
    if (!slice_active_[index]) return NdsMachineStatus::NoActiveSlice;

    oracis_scheduler_end_slice(cpu);
    slice_active_[index] = false;

    const uint64_t scheduler_cycles = oracis_scheduler_cycle_count(cpu);
    if (idle_core_cycles_offset_[index] >
        std::numeric_limits<uint64_t>::max() - scheduler_cycles) {
        return NdsMachineStatus::ClockReportFailed;
    }
    const uint64_t cumulative_core_cycles =
        scheduler_cycles + idle_core_cycles_offset_[index];

    return oracis_bus_report_cpu_cycles(cpu, cumulative_core_cycles)
        ? NdsMachineStatus::Ok
        : NdsMachineStatus::ClockReportFailed;
}

NdsMachineStatus NdsMachine::report_idle_bus_cycles(
    NdsCpu cpu, uint64_t elapsed_bus_cycles) noexcept
{
    if (!initialized_) return NdsMachineStatus::NotInitialized;
    if (!valid_cpu(cpu)) return NdsMachineStatus::InvalidCpu;

    const uint64_t core_cycles_per_bus_tick = cpu == NDS_ARM9 ? 2u : 1u;
    if (elapsed_bus_cycles >
        std::numeric_limits<uint64_t>::max() / core_cycles_per_bus_tick) {
        return NdsMachineStatus::ClockReportFailed;
    }

    return report_idle_core_cycles(
        cpu, elapsed_bus_cycles * core_cycles_per_bus_tick);
}

NdsMachineStatus NdsMachine::report_idle_core_cycles(
    NdsCpu cpu, uint64_t elapsed_core_cycles) noexcept
{
    if (!initialized_) return NdsMachineStatus::NotInitialized;
    if (!valid_cpu(cpu)) return NdsMachineStatus::InvalidCpu;
    if (slice_active_[0] || slice_active_[1]) {
        return NdsMachineStatus::SliceAlreadyActive;
    }
    if (elapsed_core_cycles == 0u) return NdsMachineStatus::Ok;

    const unsigned index = cpu_index(cpu);
    const uint64_t scheduler_cycles = oracis_scheduler_cycle_count(cpu);
    if (idle_core_cycles_offset_[index] >
        std::numeric_limits<uint64_t>::max() - scheduler_cycles) {
        return NdsMachineStatus::ClockReportFailed;
    }

    const uint64_t reported_cycles =
        scheduler_cycles + idle_core_cycles_offset_[index];
    if (elapsed_core_cycles >
        std::numeric_limits<uint64_t>::max() - reported_cycles ||
        elapsed_core_cycles >
        std::numeric_limits<uint64_t>::max() -
            idle_core_cycles_offset_[index]) {
        return NdsMachineStatus::ClockReportFailed;
    }

    const uint64_t next_reported_cycles =
        reported_cycles + elapsed_core_cycles;
    if (!oracis_bus_report_cpu_cycles(cpu, next_reported_cycles)) {
        return NdsMachineStatus::ClockReportFailed;
    }

    idle_core_cycles_offset_[index] += elapsed_core_cycles;
    return NdsMachineStatus::Ok;
}

NdsMachineStatus NdsMachine::run_dual_core_round(
    uint64_t bus_cycle_budget,
    NdsMachineSliceExecutor execute_slice,
    NdsMachineIrqContextProvider irq_context,
    void *context) noexcept
{
    if (!initialized_) return NdsMachineStatus::NotInitialized;
    if (execute_slice == nullptr) return NdsMachineStatus::SliceExecutionFailed;
    if (bus_cycle_budget == 0u) return NdsMachineStatus::InvalidCycleBudget;

    const uint64_t current_bus_cycles = system_clock_.global_bus_cycles();
    if (bus_cycle_budget >
            std::numeric_limits<uint64_t>::max() - current_bus_cycles) {
        return NdsMachineStatus::InvalidCycleBudget;
    }

    const uint64_t target_bus_cycles =
        current_bus_cycles + bus_cycle_budget;
    if (target_bus_cycles > std::numeric_limits<uint64_t>::max() / 2u) {
        return NdsMachineStatus::InvalidCycleBudget;
    }

    const std::array<NdsCpu, 2> order = next_round_first_cpu_ == NDS_ARM9
        ? std::array<NdsCpu, 2>{NDS_ARM9, NDS_ARM7}
        : std::array<NdsCpu, 2>{NDS_ARM7, NDS_ARM9};

    const auto service_irq = [this, irq_context, context](
        NdsCpu cpu) noexcept -> NdsMachineStatus {
        if (!cpu_can_execute(cpu)) return NdsMachineStatus::Ok;

        const ArmCpuState *state = cpu_state(cpu);
        if (state == nullptr) return NdsMachineStatus::InvalidCpu;
        if (!interrupt_control_.irq_asserted(cpu) ||
            (state->cpsr & CPSR_I_BIT) != 0u) {
            return NdsMachineStatus::Ok;
        }
        if (irq_context == nullptr) {
            return NdsMachineStatus::IrqContextUnavailable;
        }

        uint32_t return_address = 0u;
        uint32_t legacy_vector_base = 0u;
        bool supplied = false;
        try {
            supplied = irq_context(
                context, cpu, state, &return_address, &legacy_vector_base);
        } catch (...) {
            return NdsMachineStatus::IrqContextFailed;
        }
        if (!supplied) return NdsMachineStatus::IrqContextFailed;

        /* Do not trust a host guess: select from the modeled CPU CP15 state. */
        const uint32_t vector_base = exception_vector_base(cpu);
        const NdsMachineStatus result = try_deliver_pending_irq(
            cpu, return_address, vector_base);
        return result == NdsMachineStatus::IrqEntered
            ? NdsMachineStatus::Ok
            : result;
    };

    /* Service requests already pending at the start of this safe round. */
    for (const NdsCpu cpu : order) {
        const NdsMachineStatus result = service_irq(cpu);
        if (result != NdsMachineStatus::Ok) return result;
    }

    const uint64_t target_arm9_cycles = target_bus_cycles * 2u;
    const std::array<uint64_t, 2> target_cycles = {
        target_arm9_cycles,
        target_bus_cycles
    };

    for (const NdsCpu cpu : order) {
        const unsigned index = cpu_index(cpu);
        const uint64_t current_cycles =
            system_clock_.cpu_cycle_count(cpu);
        const uint64_t target = target_cycles[index];
        if (current_cycles >= target) continue;

        const uint64_t remaining_cycles = target - current_cycles;
        if (cpu_can_execute(cpu)) {
            const NdsMachineStatus begin =
                begin_cpu_slice(cpu, remaining_cycles);
            if (begin != NdsMachineStatus::Ok) return begin;

            bool executed = false;
            try {
                executed = execute_slice(
                    context, cpu, cpu_state(cpu), remaining_cycles);
            } catch (...) {
                executed = false;
            }

            const NdsMachineStatus end = end_cpu_slice(cpu);
            if (end != NdsMachineStatus::Ok) return end;
            if (!executed) return NdsMachineStatus::SliceExecutionFailed;
        }

        /*
         * A halted core or a core that yielded before the common watermark
         * still advances in elapsed clock time. Keep the exact core-cycle
         * remainder, including ARM9 half-bus ticks, instead of rounding.
         */
        const uint64_t after_execution =
            system_clock_.cpu_cycle_count(cpu);
        if (after_execution < target) {
            const NdsMachineStatus idle = report_idle_core_cycles(
                cpu, target - after_execution);
            if (idle != NdsMachineStatus::Ok) return idle;
        }
    }

    /* Timer/device IRQs raised while the common watermark advanced are now
       visible to both cores and are entered only at this safe boundary. */
    for (const NdsCpu cpu : order) {
        const NdsMachineStatus result = service_irq(cpu);
        if (result != NdsMachineStatus::Ok) return result;
    }

    next_round_first_cpu_ =
        next_round_first_cpu_ == NDS_ARM9 ? NDS_ARM7 : NDS_ARM9;
    return NdsMachineStatus::Ok;
}

bool NdsMachine::execute_registered_bank_slice(
    void *context, NdsCpu cpu, ArmCpuState *state,
    uint64_t cycle_budget)
{
    (void)context;
    (void)cycle_budget;

    if (!valid_cpu(cpu) || state == nullptr ||
        oracis_active_cpu_state != state || g_nds_active != cpu) {
        return false;
    }

    /*
     * A dispatch row is one translated guest block, not necessarily a whole
     * scheduling quantum. Keep dispatching subsequent guest PCs until the
     * generated code requests a safe yield (budget, HALT, or explicit yield).
     */
    while (!runtime_should_yield()) {
        runtime_dispatch(state->R[15]);
    }
    return true;
}

NdsMachineStatus NdsMachine::enter_cpu_halt(NdsCpu cpu) noexcept
{
    if (!initialized_) return NdsMachineStatus::NotInitialized;
    if (!valid_cpu(cpu)) return NdsMachineStatus::InvalidCpu;
    const unsigned index = cpu_index(cpu);
    if (slice_active_[1u - index]) {
        return NdsMachineStatus::SliceAlreadyActive;
    }

    const bool interrupt_pending = cpu == NDS_ARM7
        ? interrupt_control_.pending_enabled(cpu) != 0u
        : interrupt_control_.irq_asserted(cpu);

    /* A level already active at the HALT boundary prevents the CPU sleeping. */
    run_states_[index] = interrupt_pending
        ? NdsCpuRunState::Running
        : NdsCpuRunState::Halted;

    /* A HALT requested inside a generated bank exits at its next yield check. */
    if (!interrupt_pending && slice_active_[index] &&
        !oracis_scheduler_request_yield(cpu)) {
        run_states_[index] = NdsCpuRunState::Running;
        return NdsMachineStatus::SchedulerFailure;
    }
    return NdsMachineStatus::Ok;
}

NdsMachineStatus NdsMachine::write_arm7_halt_control(uint8_t value) noexcept
{
    if (!initialized_) return NdsMachineStatus::NotInitialized;

    /* NDS7 HALTCNT bits 6-7: 0=no-op, 1=GBA mode, 2=HALT, 3=SLEEP. */
    switch ((value >> 6u) & 0x3u) {
    case 0u:
        return NdsMachineStatus::Ok;
    case 1u: /* GBA compatibility mode is outside the NDS runtime target. */
    case 3u: /* SLEEP also suspends hardware domains not modeled by Oracis. */
        return NdsMachineStatus::UnsupportedPowerMode;
    case 2u:
        return enter_cpu_halt(NDS_ARM7);
    }
    return NdsMachineStatus::UnsupportedPowerMode;
}

bool NdsMachine::cpu_can_execute(NdsCpu cpu) noexcept
{
    if (!initialized_ || !valid_cpu(cpu)) return false;

    const unsigned index = cpu_index(cpu);
    if (run_states_[index] == NdsCpuRunState::Running) return true;

    /*
     * NDS7 HALT wakes on IE&IF, independently of IME and CPSR.I.
     * NDS9 Wait-For-Interrupt requires the IME-gated IRQ line to be asserted;
     * CPSR.I controls exception entry, not whether the core leaves HALT.
     */
    const bool interrupt_pending = cpu == NDS_ARM7
        ? interrupt_control_.pending_enabled(cpu) != 0u
        : interrupt_control_.irq_asserted(cpu);

    if (interrupt_pending) {
        run_states_[index] = NdsCpuRunState::Running;
        return true;
    }
    return false;
}

bool NdsMachine::cpu_is_halted(NdsCpu cpu) const noexcept
{
    return initialized_ && valid_cpu(cpu) &&
        run_states_[cpu_index(cpu)] == NdsCpuRunState::Halted;
}

NdsMachineStatus NdsMachine::try_deliver_pending_irq(
    NdsCpu cpu, uint32_t return_address, uint32_t vector_base) noexcept
{
    if (!initialized_) return NdsMachineStatus::NotInitialized;
    if (!valid_cpu(cpu)) return NdsMachineStatus::InvalidCpu;
    if (slice_active_[0] || slice_active_[1]) {
        return NdsMachineStatus::SliceAlreadyActive;
    }

    /* First apply the core-specific HALT wake rule. */
    if (!cpu_can_execute(cpu)) return NdsMachineStatus::CpuHalted;

    ArmCpuState *state = cpu_state(cpu);
    if (state == nullptr) return NdsMachineStatus::InvalidCpu;

    /* IE/IF/IME and CPSR.I are separate gates: wake does not imply IRQ entry. */
    if (!interrupt_control_.irq_asserted(cpu) ||
        (state->cpsr & CPSR_I_BIT) != 0u) {
        return NdsMachineStatus::IrqNotEligible;
    }

    if (!oracis_arm_runtime_select_cpu(cpu)) {
        return NdsMachineStatus::SchedulerFailure;
    }

    return interrupt_control_.try_enter_irq(
        cpu, *state, return_address, vector_base)
        ? NdsMachineStatus::IrqEntered
        : NdsMachineStatus::IrqEntryFailed;
}

uint32_t NdsMachine::exception_vector_base(NdsCpu cpu) const noexcept
{
    if (cpu == NDS_ARM9) {
        constexpr uint32_t kAlternateVectorSelect = UINT32_C(1) << 13u;
        return (arm9_cp15_control_ & kAlternateVectorSelect) != 0u
            ? UINT32_C(0xFFFF0000)
            : 0u;
    }
    return 0u;
}

ArmCpuState *NdsMachine::cpu_state(NdsCpu cpu) noexcept
{
    if (!valid_cpu(cpu)) return nullptr;
    return cpu == NDS_ARM9 ? &arm9_state_ : &arm7_state_;
}

const ArmCpuState *NdsMachine::cpu_state(NdsCpu cpu) const noexcept
{
    if (!valid_cpu(cpu)) return nullptr;
    return cpu == NDS_ARM9 ? &arm9_state_ : &arm7_state_;
}

} // namespace oracis::machine
