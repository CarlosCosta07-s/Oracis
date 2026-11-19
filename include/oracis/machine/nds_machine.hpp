/*
 * Oracis Nintendo DS machine context.
 *
 * Owns both CPU states and the base memory/MMIO devices, and exposes a
 * synchronized dual-core scheduling round. The complete DS event scheduler
 * and integration with the SDL host remain separate implementation steps.
 */
#ifndef ORACIS_MACHINE_NDS_MACHINE_HPP
#define ORACIS_MACHINE_NDS_MACHINE_HPP

#include <array>
#include <cstdint>

#include "oracis/recomp/arm_cpu_state.h"
#include "oracis/memory/guest_memory.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"
#include "oracis/memory/nds_keypad_control.hpp"
#include "oracis/memory/nds_retail_map.hpp"
#include "oracis/memory/nds_system_clock.hpp"
#include "oracis/memory/nds_timer_control.hpp"
#include "oracis/memory/nds_wram_control.hpp"

/* Forward-declared here to keep the machine header independent of runtime_arm.h. */
struct NdsStaticValidation;

namespace oracis::machine {

enum class NdsCpuRunState : uint8_t {
    Running = 0,
    Halted
};

enum class NdsMachineStatus : uint8_t {
    Ok = 0,
    AlreadyInitialized,
    RuntimeAlreadyBound,
    MemoryMapFailed,
    BusBindingFailed,
    NotInitialized,
    InvalidCpu,
    SliceAlreadyActive,
    NoActiveSlice,
    SchedulerFailure,
    ClockReportFailed,
    CpuHalted,
    IrqNotEligible,
    IrqEntered,
    IrqEntryFailed,
    UnsupportedPowerMode,
    InvalidCycleBudget,
    SliceExecutionFailed,
    IrqContextUnavailable,
    IrqContextFailed,
    ProgramAlreadyLoaded,
    InvalidProgramImage,
    ProgramMemoryUnavailable,
    ProgramLoadAfterExecution,
    ProgramLoadFailed,
    ShutdownFailed
};

/*
 * Executes one cooperative native-code slice for the selected CPU.
 * The machine has already selected the CPU and opened its scheduler budget.
 * The callback must return at a generated-code yield/safe instruction boundary.
 */
using NdsMachineSliceExecutor = bool (*)(
    void *context, NdsCpu cpu, ArmCpuState *state,
    uint64_t cycle_budget);

/*
 * Supplies architecture-specific IRQ state. The callback must calculate the
 * IRQ LR for the exact boundary and select the active low/high vector base.
 */
using NdsMachineIrqContextProvider = bool (*)(
    void *context, NdsCpu cpu, const ArmCpuState *state,
    uint32_t *return_address, uint32_t *vector_base);

/*
 * One already-extracted NDS executable segment. entry_pc is the aligned
 * address with the Thumb marker removed; entry_thumb carries CPSR.T.
 */
struct NdsProgramSegment {
    NdsCpu cpu = NDS_ARM9;
    uint32_t load_address = 0u;
    uint32_t size = 0u;
    uint32_t entry_pc = 0u;
    bool entry_thumb = false;
    const uint8_t *contents = nullptr;
};

class NdsMachine final {
public:
    explicit NdsMachine(
        memory::SharedWramMode initial_wram_mode) noexcept;
    ~NdsMachine();

    NdsMachine(const NdsMachine &) = delete;
    NdsMachine &operator=(const NdsMachine &) = delete;
    NdsMachine(NdsMachine &&) = delete;
    NdsMachine &operator=(NdsMachine &&) = delete;

    /*
     * Installs retail RAM and binds this machine as the process-wide active
     * runtime. Only one NdsMachine may be bound at a time.
     */
    NdsMachineStatus initialize() noexcept;

    /*
     * Preflight and load one ARM9 plus one ARM7 segment into existing retail
     * executable RAM mappings. This materializes the bytes, validates their
     * identity, and seeds PC/CPSR.T. It does not reproduce BIOS register/stack
     * setup or imply that missing NitroSDK/timing services are ready to run.
     */
    NdsMachineStatus load_program_segments(
        const NdsProgramSegment &arm9,
        const NdsProgramSegment &arm7) noexcept;
    bool program_loaded() const noexcept { return program_loaded_; }

    /* Unbinds devices in reverse dependency order. Safe to call more than once. */
    NdsMachineStatus shutdown() noexcept;

    bool initialized() const noexcept { return initialized_; }

    /*
     * Slice boundary helpers. begin_cpu_slice selects the CPU and starts its
     * cooperative budget. end_cpu_slice reports its cumulative scheduler
     * counter to NdsSystemClock, which advances peripherals only at the
     * common ARM9/ARM7 watermark.
     */
    NdsMachineStatus begin_cpu_slice(
        NdsCpu cpu, uint64_t cycle_budget) noexcept;
    NdsMachineStatus end_cpu_slice(NdsCpu cpu) noexcept;

    /*
     * Explicitly account bus time for a CPU which is not executing guest
     * instructions while its clock domain remains active. Do not use this for
     * STOP or any state where that CPU's clock is stopped.
     */
    NdsMachineStatus report_idle_bus_cycles(
        NdsCpu cpu, uint64_t elapsed_bus_cycles) noexcept;

    /*
     * Account core cycles during a period in which this CPU is not executing
     * guest instructions but its clock domain remains active. This preserves
     * half-bus-cycle precision for ARM9.
     */
    NdsMachineStatus report_idle_core_cycles(
        NdsCpu cpu, uint64_t elapsed_core_cycles) noexcept;

    /*
     * Advance both CPU timelines toward one common bus-clock watermark.
     * ARM9 targets two core cycles per bus cycle; ARM7 targets one. A CPU that
     * is HALTed or yields early is accounted as idle for the rest of this
     * scheduling quantum. The callback executes native generated code; this
     * method does not emulate missing devices or invent IRQ return addresses.
     */
    NdsMachineStatus run_dual_core_round(
        uint64_t bus_cycle_budget,
        NdsMachineSliceExecutor execute_slice,
        NdsMachineIrqContextProvider irq_context,
        void *context) noexcept;

    /*
     * Default executor for registered static banks. It dispatches the
     * selected CPU's current guest PC through runtime_dispatch(); a missing
     * or invalid bank remains a fatal, fail-closed dispatch error.
     */
    static bool execute_registered_bank_slice(
        void *context, NdsCpu cpu, ArmCpuState *state,
        uint64_t cycle_budget);

    /*
     * HALT is modeled as a CPU execution state; peripherals keep advancing
     * through the synchronized bus clock. IRQ delivery remains a separate
     * scheduler operation. ARM7 HALTCNT accepts 0x80 for HALT; GBA-mode and
     * Sleep requests are deliberately rejected until their hardware effects
     * can be modeled.
     */
    NdsMachineStatus enter_cpu_halt(NdsCpu cpu) noexcept;
    NdsMachineStatus write_arm7_halt_control(uint8_t value) noexcept;
    bool cpu_can_execute(NdsCpu cpu) noexcept;
    bool cpu_is_halted(NdsCpu cpu) const noexcept;

    /*
     * Attempt IRQ entry only at a safe inter-slice boundary. return_address
     * must already contain the architecture-correct IRQ LR (the machine never
     * guesses ARM/Thumb pipeline adjustment); vector_base is the active vector
     * base selected by the caller/CP15 state (low or high vectors).
     */
    NdsMachineStatus try_deliver_pending_irq(
        NdsCpu cpu, uint32_t return_address,
        uint32_t vector_base) noexcept;

    uint64_t global_bus_cycles() const noexcept {
        return system_clock_.global_bus_cycles();
    }

    memory::GuestMemory &guest_memory() noexcept { return memory_; }
    memory::NdsWramControl &wram_control() noexcept { return wram_control_; }
    memory::NdsInterruptControl &interrupt_control() noexcept {
        return interrupt_control_;
    }
    memory::NdsTimerControl &timer_control() noexcept { return timer_control_; }
    memory::NdsKeypadControl &keypad_control() noexcept {
        return keypad_control_;
    }
    memory::NdsSystemClock &system_clock() noexcept { return system_clock_; }

    ArmCpuState *cpu_state(NdsCpu cpu) noexcept;
    const ArmCpuState *cpu_state(NdsCpu cpu) const noexcept;

private:
    static bool valid_cpu(NdsCpu cpu) noexcept;
    static unsigned cpu_index(NdsCpu cpu) noexcept;
    static int runtime_power_handler(
        void *context, NdsCpu cpu,
        uint32_t operation, uint32_t value);
    static int runtime_validate_dispatch_bytes(
        NdsCpu cpu, const NdsStaticValidation *validation,
        uint32_t pc, uint32_t thumb, void *context);

    memory::GuestMemory memory_;
    ArmCpuState arm9_state_{};
    ArmCpuState arm7_state_{};
    memory::NdsWramControl wram_control_;
    memory::NdsInterruptControl interrupt_control_;
    memory::NdsTimerControl timer_control_;
    memory::NdsKeypadControl keypad_control_;
    memory::NdsSystemClock system_clock_;

    std::array<bool, 2> slice_active_{};
    std::array<NdsCpuRunState, 2> run_states_{};
    /* Core-cycle equivalents already reported through explicit idle time. */
    std::array<uint64_t, 2> idle_core_cycles_offset_{};
    NdsCpu next_round_first_cpu_ = NDS_ARM9;
    bool runtime_bound_ = false;
    bool power_handler_bound_ = false;
    bool memory_bound_ = false;
    bool wram_bound_ = false;
    bool interrupt_bound_ = false;
    bool timer_bound_ = false;
    bool keypad_bound_ = false;
    bool clock_bound_ = false;
    bool validation_bound_ = false;
    bool program_loaded_ = false;
    bool initialized_ = false;
};

} // namespace oracis::machine

#endif /* ORACIS_MACHINE_NDS_MACHINE_HPP */
