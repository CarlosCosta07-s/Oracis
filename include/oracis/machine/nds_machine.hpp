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
#include "oracis/machine/nds_overlay_loader.hpp"
#include "oracis/memory/nds_arm9_tcm.hpp"
#include "oracis/memory/nds_arm9_protection_unit.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"
#include "oracis/memory/nds_external_memory_control.hpp"
#include "oracis/memory/nds_ipc_control.hpp"
#include "oracis/memory/nds_game_card_control.hpp"
#include "oracis/memory/nds_bios_control.hpp"
#include "oracis/memory/nds_game_card_key1.hpp"
#include "oracis/memory/nds_dma_control.hpp"
#include "oracis/memory/nds_keypad_control.hpp"
#include "oracis/memory/nds_retail_map.hpp"
#include "oracis/memory/nds_system_clock.hpp"
#include "oracis/memory/nds_timer_control.hpp"
#include "oracis/memory/nds_wram_control.hpp"
#include "oracis/memory/nds_vram_control.hpp"
#include "oracis/memory/nds_video_memory.hpp"
#include "oracis/memory/nds_post_flag.hpp"
#include "oracis/memory/nds_rtc.hpp"
#include "oracis/memory/nds_spi_control.hpp"
#include "oracis/memory/nds_display_control.hpp"
#include "oracis/memory/nds_geometry_engine.hpp"
#include "oracis/memory/nds_3d_rasterizer.hpp"

/* Forward-declared here to keep the machine header independent of runtime_arm.h. */
struct NdsStaticValidation;

namespace oracis::machine {

enum class NdsCpuRunState : uint8_t {
    Running = 0,
    Halted,
    ResetHeld
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
    ShutdownFailed,
    OverlayInitializerFailed,
    FirmwareBootCodeFailed,
    FirmwareBootMemoryUnavailable,
    CpuResetHeld,
    CpuNotInReset,
    RetailBiosBootUnavailable,
    RetailBiosBootAlreadyActive,
    RetailBiosBootActive
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
 * Supplies the architecture-correct IRQ LR for the exact boundary.
 * The legacy vector_base output is retained for API compatibility; NdsMachine
 * selects the real base from ARM9 CP15 Control.V (ARM7 always uses zero).
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
     * Enter the hardware-reset BIOS path using user-supplied ARM9/ARM7 BIOS
     * images. ARM9 starts at 0xFFFF0000; ARM7 remains held in reset until the
     * host explicitly releases it after a verified hardware hand-off.
     *
     * This establishes CPU reset state only. It does not synthesize BIOS
     * banks, execute BIOS code, or infer an IPCSYNC release value.
     */
    NdsMachineStatus start_retail_bios_boot() noexcept;
    NdsMachineStatus release_arm7_from_reset() noexcept;
    bool retail_bios_boot_active() const noexcept {
        return retail_bios_boot_active_;
    }

    /*
     * Preflight and load one ARM9 plus one ARM7 segment into existing retail
     * executable RAM mappings. When supplied, cartridge_header must contain
     * exactly the 0x170 bytes mirrored by NDS boot at 0x027FFE00.
     * Seeds the documented cartridge-entry R12/R13/R14/R15, IRQ/SVC stacks
     * and POSTFLG handoff state. It does not execute the BIOS/firmware boot
     * sequence or establish that missing NitroSDK/timing services are ready.
     */
    NdsMachineStatus load_program_segments(
        const NdsProgramSegment &arm9,
        const NdsProgramSegment &arm7,
        const uint8_t *cartridge_header = nullptr,
        uint32_t cartridge_header_size = 0u) noexcept;
    bool program_loaded() const noexcept { return program_loaded_; }

    /*
     * Decrypt/decompress the user-supplied SPI firmware IPL parts with the
     * BIOS-derived KEY1 table and install them into their validated guest RAM
     * destinations. Must run before program entry/execution.
     */
    NdsMachineStatus load_firmware_boot_code() noexcept;
    bool firmware_boot_code_loaded() const noexcept {
        return firmware_boot_code_loaded_;
    }
    const memory::NdsFirmwareBootStaging *firmware_boot_staging() const noexcept {
        return firmware_boot_code_loaded_ ? &firmware_boot_staging_ : nullptr;
    }

    /* Overlay images are supplied already extracted and decompressed. */
    NdsOverlayStatus load_overlay(const NdsOverlayImage &image) noexcept;
    /*
     * Execute the validated static-init callbacks once, in NDS table order.
     * Each callback receives a bounded guest slice and a synthetic guest
     * return frame. CPU registers and call-return stack are restored after
     * every callback; guest memory/device side effects are retained.
     */
    NdsOverlayStatus run_overlay_static_initializers(
        NdsCpu cpu, uint32_t overlay_id,
        uint64_t cycle_budget_per_initializer) noexcept;
    /*
     * Request static initialization from an HLE/guest callback. When called
     * inside the selected CPU's active slice, this records one pending request
     * and asks the generated code to unwind at its next checkpoint. The
     * constructors run only after the host closes that slice. Outside a slice,
     * the request is executed immediately at the already-safe boundary.
     */
    NdsOverlayStatus request_overlay_static_initializers(
        NdsCpu cpu, uint32_t overlay_id,
        uint64_t cycle_budget_per_initializer) noexcept;
    NdsOverlayStatus unload_overlay(
        NdsCpu cpu, uint32_t overlay_id,
        bool no_live_guest_references) noexcept;
    const NdsOverlayLoader &overlay_loader() const noexcept {
        return overlay_loader_;
    }

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

    /* Prepare Engine A's rolling 48-line 3D BG0 lookahead window. */
    memory::Nds3dRasterStatus prepare_3d_bg0_scanline(
        uint16_t scanline) noexcept;
    memory::Nds3dScanlineCache &three_d_scanline_cache() noexcept {
        return three_d_scanline_cache_;
    }

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
    bool cpu_is_held_in_reset(NdsCpu cpu) const noexcept;

    /*
     * Attempt IRQ entry only at a safe inter-slice boundary. return_address
     * must already contain the architecture-correct IRQ LR (the machine never
     * guesses ARM/Thumb pipeline adjustment); vector_base is the active vector
     * base selected by the caller/CP15 state (low or high vectors).
     */
    NdsMachineStatus try_deliver_pending_irq(
        NdsCpu cpu, uint32_t return_address,
        uint32_t vector_base) noexcept;

    /*
     * Accept an IRQ at a generated-code boundary. return_pc is the next guest
     * PC; the machine derives LR_irq = return_pc + 4 and chooses the vector.
     * If a CPU slice is active, it must belong to this CPU.
     */
    NdsMachineStatus enter_runtime_irq(
        NdsCpu cpu, uint32_t return_pc) noexcept;
    bool cpu_slice_active(NdsCpu cpu) const noexcept;

    /* ARM9 CP15 Control.V selects high exception vectors; ARM7 is fixed low. */
    uint32_t exception_vector_base(NdsCpu cpu) const noexcept;

    uint64_t global_bus_cycles() const noexcept {
        return system_clock_.global_bus_cycles();
    }

    memory::GuestMemory &guest_memory() noexcept { return memory_; }
    memory::NdsArm9Tcm &arm9_tcm() noexcept { return arm9_tcm_; }
    memory::NdsWramControl &wram_control() noexcept { return wram_control_; }
    memory::NdsVramControl &vram_control() noexcept { return vram_control_; }
    memory::NdsVideoMemory &video_memory() noexcept { return video_memory_; }
    memory::NdsDisplayControl &display_control() noexcept {
        return display_control_;
    }
    memory::NdsGeometryEngine &geometry_engine() noexcept {
        return geometry_engine_;
    }
    memory::NdsExternalMemoryControl &external_memory_control() noexcept {
        return external_memory_control_;
    }
    memory::NdsInterruptControl &interrupt_control() noexcept {
        return interrupt_control_;
    }
    memory::NdsIpcControl &ipc_control() noexcept { return ipc_control_; }
    memory::NdsGameCardControl &game_card_control() noexcept { return game_card_control_; }
    memory::NdsDmaControl &dma_control() noexcept { return dma_control_; }
    memory::NdsTimerControl &timer_control() noexcept { return timer_control_; }
    memory::NdsKeypadControl &keypad_control() noexcept {
        return keypad_control_;
    }
    memory::NdsRtc &rtc() noexcept { return rtc_; }
    memory::NdsBiosControl &bios_control() noexcept { return bios_control_; }
    memory::NdsSpiControl &spi_control() noexcept { return spi_control_; }
    memory::NdsSystemClock &system_clock() noexcept { return system_clock_; }

    ArmCpuState *cpu_state(NdsCpu cpu) noexcept;
    const ArmCpuState *cpu_state(NdsCpu cpu) const noexcept;

private:
    struct PendingOverlayInitializers {
        bool pending = false;
        uint32_t overlay_id = 0u;
        uint64_t cycle_budget_per_initializer = 0u;
    };

    NdsMachineStatus run_pending_overlay_static_initializers(
        NdsCpu cpu) noexcept;

    struct OverlayDispatchMissCapture {
        NdsCpu cpu = NDS_ARM9;
        bool seen = false;
        uint32_t target_pc = 0u;
        uint32_t cpsr = 0u;
        uint32_t lookup_status = 0u;
        uint32_t candidate_count = 0u;
        uint32_t has_inactive_candidate = 0u;
    };

    static int runtime_overlay_dispatch_miss_handler(
        void *context, NdsCpu cpu, uint32_t target_pc, uint32_t cpsr,
        uint32_t lookup_status, uint32_t candidate_count,
        uint32_t has_inactive_candidate);

    static bool valid_cpu(NdsCpu cpu) noexcept;
    static unsigned cpu_index(NdsCpu cpu) noexcept;
    static void display_scanline_observer(
        void *context, uint16_t previous_scanline,
        uint16_t current_scanline, bool natural_frame_wrap) noexcept;
    static void geometry_overflow_observer(void *context) noexcept;
    void on_display_scanline_boundary(
        uint16_t previous_scanline, uint16_t current_scanline,
        bool natural_frame_wrap) noexcept;
    memory::Nds3dRasterStatus reset_3d_bg0_scanline_sequence(
        uint16_t first_scanline) noexcept;
    memory::Nds3dRasterStatus fill_3d_bg0_lookahead(
        uint16_t scanline) noexcept;
    void sync_arm9_tcm_timing_state() noexcept;
    NdsMachineStatus begin_interrupt_wait(
        NdsCpu cpu, uint32_t clear_old_flags,
        uint32_t wait_mask) noexcept;
    NdsMachineStatus resume_interrupt_wait(
        NdsCpu cpu, uint32_t return_pc) noexcept;
    NdsMachineStatus read_bios_interrupt_flags(
        NdsCpu cpu, uint32_t &flags) const noexcept;
    NdsMachineStatus write_bios_interrupt_flags(
        NdsCpu cpu, uint32_t flags) noexcept;
    static int runtime_power_handler(
        void *context, NdsCpu cpu,
        uint32_t operation, uint32_t value);
    static int runtime_irq_handler(
        void *context, NdsCpu cpu, uint32_t return_pc);
    static int runtime_interrupt_wait_handler(
        void *context, NdsCpu cpu, uint32_t operation,
        uint32_t argument0, uint32_t argument1);
    static int runtime_coprocessor_handler(
        void *context, NdsCpu cpu, uint32_t is_write,
        uint32_t cp_num, uint32_t op1, uint32_t crn,
        uint32_t crm, uint32_t op2, uint32_t *value);
    static int runtime_bus_access_check(
        void *context, NdsCpu cpu, uint32_t address, uint32_t size,
        uint32_t access_kind, uint32_t privileged);
    int check_arm9_protection_access(
        NdsCpu cpu, uint32_t address, uint32_t size,
        uint32_t access_kind, bool privileged) const noexcept;
    static int runtime_data_abort_handler(void *context, NdsCpu cpu);
    static int runtime_prefetch_abort_handler(
        void *context, NdsCpu cpu, uint32_t target_pc);
    static int runtime_bios_memory_transfer_handler(
        void *context, NdsCpu cpu, uint32_t source, uint32_t destination,
        uint32_t count, uint32_t width, uint32_t fixed_source);
    static int runtime_bios_crc16_handler(
        void *context, NdsCpu cpu, uint32_t initial_crc,
        uint32_t source, uint32_t length,
        uint32_t *result, uint32_t *last_halfword);
    static int runtime_bios_bit_unpack_handler(
        void *context, NdsCpu cpu, uint32_t source,
        uint32_t destination, uint32_t parameters);
    static int runtime_bios_lz77_handler(
        void *context, NdsCpu cpu, uint32_t source,
        uint32_t destination, uint32_t write_halfwords);
    static int runtime_bios_rle_handler(
        void *context, NdsCpu cpu, uint32_t source,
        uint32_t destination, uint32_t write_halfwords);
    static int runtime_bios_huffman_handler(
        void *context, NdsCpu cpu, uint32_t source,
        uint32_t destination, uint32_t callback_structure);
    static int runtime_bios_diff_handler(
        void *context, NdsCpu cpu, uint32_t source,
        uint32_t destination, uint32_t unit_width);
    static int runtime_validate_dispatch_bytes(
        NdsCpu cpu, const NdsStaticValidation *validation,
        uint32_t pc, uint32_t thumb, void *context);

    memory::GuestMemory memory_;
    memory::NdsBiosControl bios_control_;
    NdsOverlayLoader overlay_loader_;
    ArmCpuState arm9_state_{};
    ArmCpuState arm7_state_{};
    memory::NdsArm9Tcm arm9_tcm_;
    memory::NdsArm9ProtectionUnit arm9_protection_unit_;
    memory::NdsWramControl wram_control_;
    memory::NdsVramControl vram_control_;
    memory::NdsVideoMemory video_memory_;
    memory::NdsPostFlag post_flag_;
    memory::NdsInterruptControl interrupt_control_;
    memory::NdsExternalMemoryControl external_memory_control_;
    memory::NdsGameCardControl game_card_control_;
    memory::NdsIpcControl ipc_control_;
    memory::NdsDmaControl dma_control_;
    memory::NdsDisplayControl display_control_;
    memory::NdsGeometryEngine geometry_engine_;
    memory::Nds3dTextureSampler three_d_texture_sampler_;
    memory::Nds3dRasterizer three_d_rasterizer_;
    memory::Nds3dColorBuffer three_d_color_buffer_;
    memory::Nds3dDepthBuffer three_d_depth_buffer_;
    memory::Nds3dShadowStencilBuffer three_d_shadow_stencil_buffer_;
    memory::Nds3dScanlineCache three_d_scanline_cache_;
    uint16_t next_three_d_producer_scanline_ = 0u;
    bool three_d_scanline_sequence_initialized_ = false;
    bool three_d_edge_marking_pipeline_active_ = false;
    memory::NdsTimerControl timer_control_;
    memory::NdsKeypadControl keypad_control_;
    memory::NdsRtc rtc_;
    memory::NdsSpiControl spi_control_;
    memory::NdsSystemClock system_clock_;
    /*
     * Retain only IPL1/2 metadata after publication to GuestMemory. The
     * decoded IPL1/2 buffers are temporary; only P3/P4/P5 remain staged.
     */
    memory::NdsFirmwareBootStaging firmware_boot_staging_{};

    std::array<bool, 2> slice_active_{};
    std::array<PendingOverlayInitializers, 2> pending_overlay_initializers_{};
    std::array<NdsCpuRunState, 2> run_states_{};
    std::array<bool, 2> interrupt_wait_active_{};
    std::array<uint32_t, 2> interrupt_wait_masks_{};
    std::array<uint32_t, 2> interrupt_wait_resume_pcs_{};
    /* Core-cycle equivalents already reported through explicit idle time. */
    std::array<uint64_t, 2> idle_core_cycles_offset_{};
    NdsCpu next_round_first_cpu_ = NDS_ARM9;
    bool runtime_bound_ = false;
    bool power_handler_bound_ = false;
    bool irq_handler_bound_ = false;
    bool interrupt_wait_handler_bound_ = false;
    bool coprocessor_handler_bound_ = false;
    bool bios_memory_handler_bound_ = false;
    bool bios_crc16_handler_bound_ = false;
    bool bios_bit_unpack_handler_bound_ = false;
    bool bios_lz77_handler_bound_ = false;
    bool bios_rle_handler_bound_ = false;
    bool bios_huffman_handler_bound_ = false;
    bool bios_diff_handler_bound_ = false;
    bool memory_bound_ = false;
    bool bios_bound_ = false;
    bool arm9_access_guard_bound_ = false;
    bool data_abort_handler_bound_ = false;
    bool prefetch_abort_handler_bound_ = false;
    bool arm9_data_abort_pending_ = false;
    ArmCpuState arm9_data_abort_snapshot_{};
    uint32_t arm9_data_abort_address_ = 0u;
    uint32_t arm9_data_abort_size_ = 0u;
    uint32_t arm9_data_abort_access_kind_ = 0u;
    bool wram_bound_ = false;
    bool vram_bound_ = false;
    bool video_memory_bound_ = false;
    bool post_flag_bound_ = false;
    bool display_control_bound_ = false;
    bool geometry_engine_bound_ = false;
    bool interrupt_bound_ = false;
    bool external_memory_bound_ = false;
    bool game_card_bound_ = false;
    bool ipc_bound_ = false;
    bool dma_bound_ = false;
    bool timer_bound_ = false;
    bool keypad_bound_ = false;
    bool rtc_bound_ = false;
    bool spi_bound_ = false;
    bool clock_bound_ = false;
    bool validation_bound_ = false;
    bool program_loaded_ = false;
    bool firmware_boot_code_loaded_ = false;
    bool retail_bios_boot_active_ = false;
    bool initialized_ = false;
};

} // namespace oracis::machine

#endif /* ORACIS_MACHINE_NDS_MACHINE_HPP */