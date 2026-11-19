/*
 * Oracis Nintendo DS machine context.
 *
 * Owns the CPU states and the initial set of memory/MMIO devices, and binds
 * them to the generated-bank C ABI. This is not yet the dual-core event loop.
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
    UnsupportedPowerMode,
    ShutdownFailed
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
    bool runtime_bound_ = false;
    bool power_handler_bound_ = false;
    bool memory_bound_ = false;
    bool wram_bound_ = false;
    bool interrupt_bound_ = false;
    bool timer_bound_ = false;
    bool keypad_bound_ = false;
    bool clock_bound_ = false;
    bool initialized_ = false;
};

} // namespace oracis::machine

#endif /* ORACIS_MACHINE_NDS_MACHINE_HPP */
