#include "oracis/machine/nds_machine.hpp"

#include "oracis/memory/guest_memory_abi.h"
#include "oracis/recomp/runtime_arm.h"
#include "oracis/recomp/arm_cpu_state_ops.h"
#include "oracis/recomp/runtime_scheduler.h"
#include "oracis/recomp/static_dispatch_abi.h"

#include <cstdio>
#include <exception>
#include <limits>
#include <new>
#include <vector>

namespace oracis::machine {

NdsMachine::NdsMachine(memory::SharedWramMode initial_wram_mode) noexcept
    : arm9_tcm_(memory_),
      wram_control_(memory_, initial_wram_mode),
      vram_control_(memory_),
      video_memory_(memory_),
      interrupt_control_(memory_),
      game_card_control_(interrupt_control_),
      ipc_control_(interrupt_control_),
      dma_control_(memory_, interrupt_control_),
      display_control_(interrupt_control_, &dma_control_),
      geometry_engine_(interrupt_control_),
      three_d_texture_sampler_(vram_control_),
      three_d_rasterizer_(geometry_engine_, three_d_texture_sampler_),
      timer_control_(memory_, interrupt_control_),
      keypad_control_(memory_, interrupt_control_),
      system_clock_(
          timer_control_, display_control_, &game_card_control_,
          &geometry_engine_)
{
    dma_control_.attach_display_control(display_control_);
    dma_control_.attach_game_card_control(game_card_control_);
    game_card_control_.attach_dma_control(dma_control_);
    display_control_.set_scanline_observer(
        this, &NdsMachine::display_scanline_observer);
    geometry_engine_.set_overflow_observer(
        this, &NdsMachine::geometry_overflow_observer);
}

void NdsMachine::geometry_overflow_observer(void *context) noexcept
{
    auto *machine = static_cast<NdsMachine *>(context);
    if (machine != nullptr) {
        machine->display_control_.report_3d_polygon_vertex_ram_overflow();
    }
}

void NdsMachine::display_scanline_observer(
    void *context, uint16_t previous_scanline,
    uint16_t current_scanline, bool natural_frame_wrap) noexcept
{
    auto *machine = static_cast<NdsMachine *>(context);
    if (machine != nullptr) {
        machine->on_display_scanline_boundary(
            previous_scanline, current_scanline, natural_frame_wrap);
    }
}

void NdsMachine::on_display_scanline_boundary(
    uint16_t previous_scanline, uint16_t current_scanline,
    bool natural_frame_wrap) noexcept
{
    const bool sequential_scanline =
        previous_scanline < memory::NdsDisplayControl::LastScanline &&
        current_scanline == static_cast<uint16_t>(previous_scanline + 1u);
    const bool bg0_three_d_enabled =
        (display_control_.dispcnt(0u) & UINT32_C(0x00000108)) ==
        UINT32_C(0x00000108);

    /*
     * VCOUNT can be redirected by software. Such a jump is not a completed
     * scanline and must not finalize a frame or launch the 3D pre-render.
     * Only consecutive line edges drive the scanline producer; the natural
     * 262-to-0 frame wrap is handled separately below.
     */
    if (sequential_scanline &&
        current_scanline == memory::NdsDisplayControl::VisibleScanlines) {
        std::size_t minimum_queued_lines = 0u;
        if (three_d_scanline_cache_.finish_low_water_tracking(
                minimum_queued_lines)) {
            const std::size_t adjusted_count =
                minimum_queued_lines > 2u ? minimum_queued_lines - 2u : 0u;
            display_control_.set_three_d_rendered_line_count(
                static_cast<uint8_t>(adjusted_count > 46u ? 46u : adjusted_count));
        }
    }

    constexpr uint16_t ThreeDRenderStartScanline = 214u;
    if (sequential_scanline &&
        current_scanline == ThreeDRenderStartScanline) {
        if (!bg0_three_d_enabled) {
            three_d_scanline_sequence_initialized_ = false;
            return;
        }
        if (reset_3d_bg0_scanline_sequence(0u) ==
            memory::Nds3dRasterStatus::Ok) {
            static_cast<void>(fill_3d_bg0_lookahead(0u));
        } else {
            three_d_scanline_sequence_initialized_ = false;
        }
        return;
    }

    if (natural_frame_wrap && current_scanline == 0u) {
        /* Preserve the 48 lines prepared during the preceding VBlank. */
        if (three_d_scanline_sequence_initialized_ && bg0_three_d_enabled) {
            three_d_scanline_cache_.begin_low_water_tracking();
        }
        return;
    }

    if (!sequential_scanline ||
        current_scanline >= memory::Nds3dRasterizer::ScreenHeight ||
        !three_d_scanline_sequence_initialized_ ||
        !bg0_three_d_enabled) {
        return;
    }

    /* Refill only at real line boundaries; a full FIFO naturally back-pressures the producer. */
    static_cast<void>(fill_3d_bg0_lookahead(current_scanline));
}

memory::Nds3dRasterStatus NdsMachine::reset_3d_bg0_scanline_sequence(
    uint16_t first_scanline) noexcept
{
    using memory::Nds3dRasterStatus;
    if (first_scanline >= memory::Nds3dRasterizer::ScreenHeight) {
        return Nds3dRasterStatus::InvalidScanline;
    }
    if (three_d_scanline_cache_.reset(first_scanline) !=
        memory::Nds3dScanlineCacheStatus::Ok) {
        three_d_scanline_sequence_initialized_ = false;
        return Nds3dRasterStatus::ScanlineCacheInvalid;
    }
    const Nds3dRasterStatus rear_plane =
        three_d_rasterizer_.initialize_rear_plane(
            three_d_color_buffer_, three_d_depth_buffer_, display_control_);
    if (rear_plane != Nds3dRasterStatus::Ok) {
        three_d_scanline_sequence_initialized_ = false;
        return rear_plane;
    }
    three_d_shadow_stencil_buffer_.clear();
    next_three_d_producer_scanline_ = first_scanline;
    three_d_edge_marking_pipeline_active_ =
        first_scanline == 0u && display_control_.three_d_edge_marking_enabled();
    three_d_scanline_sequence_initialized_ = true;
    return Nds3dRasterStatus::Ok;
}

memory::Nds3dRasterStatus NdsMachine::fill_3d_bg0_lookahead(
    uint16_t scanline) noexcept
{
    using memory::Nds3dRasterStatus;
    if (scanline >= memory::Nds3dRasterizer::ScreenHeight) {
        return Nds3dRasterStatus::InvalidScanline;
    }
    if (!three_d_scanline_sequence_initialized_) {
        return Nds3dRasterStatus::ScanlineCacheInvalid;
    }

    constexpr uint16_t LookaheadLines =
        static_cast<uint16_t>(memory::Nds3dScanlineCache::Capacity);
    const uint16_t last_required = static_cast<uint16_t>(
        scanline + LookaheadLines - 1u < memory::Nds3dRasterizer::ScreenHeight
            ? scanline + LookaheadLines - 1u
            : memory::Nds3dRasterizer::ScreenHeight - 1u);
    uint16_t last_render_required = last_required;
    if (three_d_edge_marking_pipeline_active_ &&
        last_render_required < memory::Nds3dRasterizer::ScreenHeight) {
        ++last_render_required;
    }
    while (next_three_d_producer_scanline_ <= last_render_required) {
        const uint16_t output_scanline = three_d_edge_marking_pipeline_active_
            ? (next_three_d_producer_scanline_ == 0u
                ? 0u
                : (next_three_d_producer_scanline_ >=
                        memory::Nds3dRasterizer::ScreenHeight
                    ? static_cast<uint16_t>(
                        memory::Nds3dRasterizer::ScreenHeight - 1u)
                    : static_cast<uint16_t>(
                        next_three_d_producer_scanline_ - 1u)))
            : next_three_d_producer_scanline_;
        const auto available = three_d_scanline_cache_.can_enqueue(
            output_scanline);
        if (available == memory::Nds3dScanlineCacheStatus::Full) {
            return Nds3dRasterStatus::Ok;
        }
        if (available != memory::Nds3dScanlineCacheStatus::Ok) {
            return Nds3dRasterStatus::ScanlineCacheInvalid;
        }
        const Nds3dRasterStatus rendered =
            three_d_rasterizer_.render_scanline_to_cache(
                next_three_d_producer_scanline_, three_d_depth_buffer_,
                three_d_color_buffer_, three_d_scanline_cache_, display_control_,
                three_d_edge_marking_pipeline_active_,
                &three_d_shadow_stencil_buffer_);
        if (rendered != Nds3dRasterStatus::Ok) return rendered;
        ++next_three_d_producer_scanline_;
    }
    return Nds3dRasterStatus::Ok;
}

memory::Nds3dRasterStatus NdsMachine::prepare_3d_bg0_scanline(
    uint16_t scanline) noexcept
{
    using memory::Nds3dRasterStatus;
    if (scanline >= memory::Nds3dRasterizer::ScreenHeight) {
        return Nds3dRasterStatus::InvalidScanline;
    }

    /* The host may first request 3D after the game enables BG0-3D mid-frame. */
    if (!three_d_scanline_sequence_initialized_ ||
        three_d_scanline_cache_.next_consumer_scanline() != scanline) {
        const Nds3dRasterStatus reset =
            reset_3d_bg0_scanline_sequence(scanline);
        if (reset != Nds3dRasterStatus::Ok) return reset;
    }
    return fill_3d_bg0_lookahead(scanline);
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

void NdsMachine::sync_arm9_tcm_timing_state() noexcept
{
    const memory::NdsArm9Tcm::TimingState state =
        arm9_tcm_.timing_state();
    oracis_runtime_set_arm9_tcm_timing_state(
        state.itcm_window_size,
        state.dtcm_base,
        state.dtcm_window_size,
        state.itcm_load_mode ? 1u : 0u,
        state.dtcm_load_mode ? 1u : 0u);
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
    if (operation == ORACIS_ARM_RUNTIME_POWER_ARM9_POSTFLG &&
        cpu == NDS_ARM9) {
        return machine->post_flag_.write_from_bios(cpu, value) ==
            memory::Status::Ok ? 1 : 0;
    }
    return 0;
}

int NdsMachine::runtime_interrupt_wait_handler(
    void *context, NdsCpu cpu, uint32_t operation,
    uint32_t argument0, uint32_t argument1)
{
    auto *machine = static_cast<NdsMachine *>(context);
    if (machine == nullptr) return -1;

    NdsMachineStatus status = NdsMachineStatus::UnsupportedPowerMode;
    if (operation == ORACIS_ARM_RUNTIME_INTERRUPT_WAIT_BEGIN) {
        status = machine->begin_interrupt_wait(
            cpu, argument0, argument1);
    } else if (operation == ORACIS_ARM_RUNTIME_INTERRUPT_WAIT_RESUME) {
        status = machine->resume_interrupt_wait(cpu, argument0);
    }
    return status == NdsMachineStatus::Ok ? 1 : -1;
}

int NdsMachine::runtime_coprocessor_handler(
    void *context, NdsCpu cpu, uint32_t is_write,
    uint32_t cp_num, uint32_t op1, uint32_t crn,
    uint32_t crm, uint32_t op2, uint32_t *value)
{
    auto *machine = static_cast<NdsMachine *>(context);
    if (machine == nullptr || !machine->initialized_ ||
        cpu != NDS_ARM9 || is_write > 1u || value == nullptr ||
        cp_num != 15u || op1 != 0u ||
        machine->cpu_state(cpu) != oracis_active_cpu_state) {
        return 0;
    }

    const uint32_t mode =
        machine->cpu_state(cpu)->cpsr & UINT32_C(0x1F);
    if (mode != UINT32_C(0x11) && mode != UINT32_C(0x12) &&
        mode != UINT32_C(0x13) && mode != UINT32_C(0x17) &&
        mode != UINT32_C(0x1B) && mode != UINT32_C(0x1F)) {
        return 0;
    }

    if (crn == 0u && crm == 0u && is_write == 0u) {
        if (op2 == 1u) {
            /* DS ARM946E-S: 4KB D-cache, 8KB I-cache, 4-way, 8 words/line. */
            *value = UINT32_C(0x0F0D2112);
            return 1;
        }
        if (op2 == 2u) {
            *value = machine->arm9_tcm_.size_register();
            return 1;
        }
        /* The ARM946E-S ID register is selected by every other Op2 value. */
        *value = UINT32_C(0x41059461);
        return 1;
    }

    if (crn == 1u && crm == 0u && op2 == 0u) {
        if (is_write != 0u) {
            const bool enable_mpu = (*value & UINT32_C(1)) != 0u;
            if (enable_mpu && !machine->arm9_protection_unit_.can_enable()) {
                return 0;
            }
            if (!machine->arm9_tcm_.set_control(*value)) return 0;
            machine->arm9_protection_unit_.set_enabled(enable_mpu);
            machine->sync_arm9_tcm_timing_state();
            return 1;
        }
        *value = machine->arm9_tcm_.control();
        return 1;
    }

    if (crn == 9u && crm == 1u && (op2 == 0u || op2 == 1u)) {
        const bool instruction = op2 == 1u;
        if (is_write != 0u) {
            if (!machine->arm9_tcm_.write_region(instruction, *value)) return 0;
            machine->sync_arm9_tcm_timing_state();
            return 1;
        }
        return machine->arm9_tcm_.read_region(instruction, *value) ? 1 : 0;
    }

    if (crn == 2u && crm == 0u && (op2 == 0u || op2 == 1u)) {
        const bool instruction = op2 == 1u;
        if (is_write != 0u) {
            return machine->arm9_protection_unit_.write_cacheable(
                instruction, *value) ? 1 : 0;
        }
        *value = machine->arm9_protection_unit_.read_cacheable(instruction);
        return 1;
    }

    if (crn == 3u && crm == 0u && op2 == 0u) {
        if (is_write != 0u) {
            return machine->arm9_protection_unit_.write_bufferable(*value)
                ? 1 : 0;
        }
        *value = machine->arm9_protection_unit_.read_bufferable();
        return 1;
    }

    if (crn == 5u && crm == 0u && op2 <= 3u) {
        const bool instruction = (op2 & 1u) != 0u;
        const bool extended = op2 >= 2u;
        if (is_write != 0u) {
            return machine->arm9_protection_unit_.write_access_permissions(
                instruction, extended, *value) ? 1 : 0;
        }
        *value = machine->arm9_protection_unit_.read_access_permissions(
            instruction, extended);
        return 1;
    }

    if (crn == 6u && crm < 8u) {
        if (is_write != 0u) {
            if (op2 != 0u ||
                !machine->arm9_protection_unit_.write_region(crm, *value)) {
                return 0;
            }
            return 1;
        }
        if (op2 != 1u) return 0;
        return machine->arm9_protection_unit_.read_region(crm, *value) ? 1 : 0;
    }

    if (crn == 7u && is_write != 0u) {
        /*
         * Guest memory is coherent and writes complete synchronously. This
         * runtime has no resident cache lines or asynchronous write buffer,
         * so the ARM946E-S maintenance operations below are ordered no-ops.
         */
        const bool flush_instruction_all =
            crm == 5u && op2 == 0u && *value == 0u;
        const bool flush_instruction_line = crm == 5u && op2 == 1u;
        const bool flush_data_all =
            crm == 6u && op2 == 0u && *value == 0u;
        const bool flush_data_line = crm == 6u && op2 == 1u;
        const bool prefetch_instruction_line =
            crm == 13u && op2 == 1u;
        const bool clean_data_line = crm == 10u && op2 == 1u;
        const bool clean_data_index = crm == 10u && op2 == 2u;
        const bool clean_invalidate_data_line = crm == 14u && op2 == 1u;
        const bool clean_invalidate_data_index = crm == 14u && op2 == 2u;
        const bool drain_write_buffer = crm == 10u && op2 == 4u;

        if (flush_instruction_all || flush_instruction_line ||
            flush_data_all || flush_data_line ||
            prefetch_instruction_line || clean_data_line ||
            clean_data_index || clean_invalidate_data_line ||
            clean_invalidate_data_index || drain_write_buffer) {
            return 1;
        }
    }

    /* MPU, cache maintenance/configuration and other CP15 registers fail closed. */
    return 0;
}

int NdsMachine::runtime_bios_memory_transfer_handler(
    void *context,
    NdsCpu cpu,
    uint32_t source,
    uint32_t destination,
    uint32_t count,
    uint32_t width,
    uint32_t fixed_source)
{
    auto *machine = static_cast<NdsMachine *>(context);
    if (machine == nullptr || !machine->initialized_ ||
        !valid_cpu(cpu) || g_nds_active != cpu ||
        machine->cpu_state(cpu) != oracis_active_cpu_state ||
        (width != 2u && width != 4u) || fixed_source > 1u) {
        return 0;
    }
    if (count == 0u) return 1;

    const uint64_t transfer_bytes =
        static_cast<uint64_t>(count) * width;
    const uint64_t source_bytes =
        fixed_source != 0u ? width : transfer_bytes;
    constexpr uint64_t GuestAddressEnd = UINT64_C(0x1'0000'0000);
    const uint64_t source_end =
        static_cast<uint64_t>(source) + source_bytes;
    const uint64_t destination_end =
        static_cast<uint64_t>(destination) + transfer_bytes;

    if ((source % width) != 0u || (destination % width) != 0u ||
        source_end > GuestAddressEnd ||
        destination_end > GuestAddressEnd ||
        source_bytes > UINT32_MAX || transfer_bytes > UINT32_MAX) {
        return 0;
    }

    /*
     * NDS ARM7 ignores CpuSet/CpuFastSet when the source starts inside its
     * protected 16 KiB BIOS window. ARM9 has no matching restriction.
     */
    constexpr uint32_t Arm7BiosEnd = UINT32_C(0x00004000);
    if (cpu == NDS_ARM7 && source < Arm7BiosEnd) {
        return 1;
    }

    const uint32_t source_span = static_cast<uint32_t>(source_bytes);
    const uint32_t destination_span =
        static_cast<uint32_t>(transfer_bytes);
    if (machine->memory_.validate_read(
            cpu, source, source_span) != memory::Status::Ok ||
        machine->memory_.validate_write(
            cpu, destination, destination_span) != memory::Status::Ok) {
        return 0;
    }

    /*
     * MMIO is not represented by GuestMemory mappings. Such transfers fail
     * during preflight instead of bypassing device-side read/write handlers.
     */
    for (uint32_t index = 0u; index < count; ++index) {
        const uint64_t source_address = fixed_source != 0u
            ? static_cast<uint64_t>(source)
            : static_cast<uint64_t>(source) +
                static_cast<uint64_t>(index) * width;
        const uint64_t destination_address =
            static_cast<uint64_t>(destination) +
            static_cast<uint64_t>(index) * width;
        const memory::ReadResult value = machine->memory_.read(
            cpu, static_cast<uint32_t>(source_address), width);
        if (!value ||
            machine->memory_.write(
                cpu, static_cast<uint32_t>(destination_address),
                width, value.value) != memory::Status::Ok) {
            return 0;
        }
    }
    return 1;
}

int NdsMachine::runtime_bios_crc16_handler(
    void *context,
    NdsCpu cpu,
    uint32_t initial_crc,
    uint32_t source,
    uint32_t length,
    uint32_t *result,
    uint32_t *last_halfword)
{
    auto *machine = static_cast<NdsMachine *>(context);
    if (machine == nullptr || !machine->initialized_ ||
        !valid_cpu(cpu) || g_nds_active != cpu ||
        machine->cpu_state(cpu) != oracis_active_cpu_state ||
        result == nullptr || last_halfword == nullptr ||
        (source & 1u) != 0u || (length & 1u) != 0u) {
        return 0;
    }

    *result = initial_crc & UINT32_C(0xFFFF);
    *last_halfword = 0u;
    if (length == 0u) return 1;

    constexpr uint64_t GuestAddressEnd = UINT64_C(0x1'0000'0000);
    const uint64_t source_end =
        static_cast<uint64_t>(source) + static_cast<uint64_t>(length);
    if (source_end > GuestAddressEnd) return 0;

    /*
     * GetCRC16 is the NDS BIOS exception to the normal ARM7 BIOS-source
     * rejection used by CpuSet. However, BIOSPROT is not modelled yet, so
     * do not expose any ARM7 BIOS bytes through this provider.
     */
    constexpr uint32_t Arm7BiosEnd = UINT32_C(0x00004000);
    if (cpu == NDS_ARM7 && source < Arm7BiosEnd) return 0;

    if (machine->memory_.validate_read(cpu, source, length) !=
        memory::Status::Ok) {
        return 0;
    }

    uint32_t crc = initial_crc & UINT32_C(0xFFFF);
    for (uint32_t offset = 0u; offset < length; ++offset) {
        const uint64_t address =
            static_cast<uint64_t>(source) + offset;
        const memory::ReadResult byte = machine->memory_.read(
            cpu, static_cast<uint32_t>(address), 1u);
        if (!byte) return 0;

        crc ^= byte.value & UINT32_C(0xFF);
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            crc = (crc >> 1u) ^
                ((crc & 1u) != 0u ? UINT32_C(0xA001) : 0u);
        }
    }

    const memory::ReadResult final_halfword = machine->memory_.read(
        cpu, static_cast<uint32_t>(source_end - 2u), 2u);
    if (!final_halfword) return 0;

    *result = crc & UINT32_C(0xFFFF);
    *last_halfword = final_halfword.value & UINT32_C(0xFFFF);
    return 1;
}

int NdsMachine::runtime_bios_bit_unpack_handler(
    void *context,
    NdsCpu cpu,
    uint32_t source,
    uint32_t destination,
    uint32_t parameters)
{
    auto *machine = static_cast<NdsMachine *>(context);
    if (machine == nullptr || !machine->initialized_ ||
        !valid_cpu(cpu) || g_nds_active != cpu ||
        machine->cpu_state(cpu) != oracis_active_cpu_state ||
        (destination & 3u) != 0u) {
        return 0;
    }

    constexpr uint64_t GuestAddressEnd = UINT64_C(0x1'0000'0000);
    const uint64_t parameters_end =
        static_cast<uint64_t>(parameters) + UINT64_C(8);
    if (parameters_end > GuestAddressEnd ||
        machine->memory_.validate_read(cpu, parameters, 8u) !=
            memory::Status::Ok) {
        return 0;
    }

    /*
     * TUnpackStruct is packed: sourceSize (LE16), sourceWidth,
     * destWidth, then dataOffset (LE32). Read bytes explicitly so the
     * parameter block itself need not be naturally aligned.
     */
    uint8_t info[8]{};
    for (uint32_t index = 0u; index < 8u; ++index) {
        const memory::ReadResult byte = machine->memory_.read(
            cpu, parameters + index, 1u);
        if (!byte) return 0;
        info[index] = static_cast<uint8_t>(byte.value);
    }

    const uint32_t source_size =
        static_cast<uint32_t>(info[0]) |
        (static_cast<uint32_t>(info[1]) << 8u);
    const uint32_t source_width = info[2];
    const uint32_t destination_width = info[3];
    const uint32_t raw_offset =
        static_cast<uint32_t>(info[4]) |
        (static_cast<uint32_t>(info[5]) << 8u) |
        (static_cast<uint32_t>(info[6]) << 16u) |
        (static_cast<uint32_t>(info[7]) << 24u);
    const bool offset_zero_values =
        (raw_offset & UINT32_C(0x80000000)) != 0u;
    const uint32_t data_offset =
        raw_offset & UINT32_C(0x7FFFFFFF);

    const bool valid_source_width =
        source_width == 1u || source_width == 2u ||
        source_width == 4u || source_width == 8u;
    const bool valid_destination_width =
        destination_width == 1u || destination_width == 2u ||
        destination_width == 4u || destination_width == 8u ||
        destination_width == 16u || destination_width == 32u;
    if (!valid_source_width || !valid_destination_width ||
        destination_width < source_width) {
        return 0;
    }

    const uint64_t source_units =
        (static_cast<uint64_t>(source_size) * 8u) / source_width;
    const uint64_t output_bits =
        source_units * static_cast<uint64_t>(destination_width);
    if ((output_bits & UINT64_C(31)) != 0u) return 0;

    const uint64_t output_bytes = output_bits / 8u;
    if (output_bytes > UINT32_MAX) return 0;
    if (source_size == 0u) return 1;

    const uint64_t source_end =
        static_cast<uint64_t>(source) + source_size;
    const uint64_t destination_end =
        static_cast<uint64_t>(destination) + output_bytes;
    if (source_end > GuestAddressEnd ||
        destination_end > GuestAddressEnd ||
        machine->memory_.validate_read(cpu, source, source_size) !=
            memory::Status::Ok ||
        machine->memory_.validate_write(
            cpu, destination, static_cast<uint32_t>(output_bytes)) !=
            memory::Status::Ok) {
        return 0;
    }

    /*
     * Stage both input and output before the first write. This preserves
     * correctness for overlapping source/destination ranges and guarantees
     * that malformed values never leave a partially expanded destination.
     */
    try {
        std::vector<uint8_t> source_data(source_size);
        for (uint32_t index = 0u; index < source_size; ++index) {
            const memory::ReadResult byte = machine->memory_.read(
                cpu, source + index, 1u);
            if (!byte) return 0;
            source_data[index] = static_cast<uint8_t>(byte.value);
        }

        const uint32_t word_count =
            static_cast<uint32_t>(output_bits / 32u);
        std::vector<uint32_t> output_words(word_count, 0u);
        const uint32_t units_per_byte = 8u / source_width;
        const uint32_t units_per_word = 32u / destination_width;
        const uint32_t source_mask =
            (UINT32_C(1) << source_width) - 1u;
        const uint64_t destination_mask = destination_width == 32u
            ? UINT64_C(0xFFFFFFFF)
            : ((UINT64_C(1) << destination_width) - 1u);

        for (uint64_t unit = 0u; unit < source_units; ++unit) {
            const uint32_t source_byte =
                source_data[static_cast<std::size_t>(unit / units_per_byte)];
            const uint32_t source_shift =
                static_cast<uint32_t>(unit % units_per_byte) * source_width;
            const uint32_t value =
                (source_byte >> source_shift) & source_mask;
            const uint64_t adjusted =
                (value != 0u || offset_zero_values)
                    ? static_cast<uint64_t>(value) + data_offset
                    : static_cast<uint64_t>(value);
            if (adjusted > destination_mask) return 0;

            const uint32_t word_index =
                static_cast<uint32_t>(unit / units_per_word);
            const uint32_t destination_shift =
                static_cast<uint32_t>(unit % units_per_word) *
                destination_width;
            output_words[word_index] |=
                static_cast<uint32_t>(adjusted) << destination_shift;
        }

        for (uint32_t index = 0u; index < word_count; ++index) {
            const uint64_t address =
                static_cast<uint64_t>(destination) +
                static_cast<uint64_t>(index) * 4u;
            if (machine->memory_.write(
                    cpu, static_cast<uint32_t>(address), 4u,
                    output_words[index]) != memory::Status::Ok) {
                return 0;
            }
        }
    } catch (const std::bad_alloc &) {
        return 0;
    }

    return 1;
}

int NdsMachine::runtime_bios_lz77_handler(
    void *context,
    NdsCpu cpu,
    uint32_t source,
    uint32_t destination,
    uint32_t write_halfwords)
{
    auto *machine = static_cast<NdsMachine *>(context);
    if (machine == nullptr || !machine->initialized_ ||
        !valid_cpu(cpu) || g_nds_active != cpu ||
        machine->cpu_state(cpu) != oracis_active_cpu_state ||
        write_halfwords > 1u || (source & 3u) != 0u ||
        (write_halfwords != 0u && (destination & 1u) != 0u)) {
        return 0;
    }

    constexpr uint64_t GuestAddressEnd = UINT64_C(0x1'0000'0000);
    const uint64_t header_end =
        static_cast<uint64_t>(source) + UINT64_C(4);
    if (header_end > GuestAddressEnd) return 0;

    const memory::ReadResult header_read =
        machine->memory_.read(cpu, source, 4u);
    if (!header_read) return 0;

    const uint32_t header = header_read.value;
    const uint32_t compressed_type = (header >> 4u) & UINT32_C(0x0F);
    const uint32_t output_size = header >> 8u;
    /*
     * BIOS LZ77 uses the 0x10 header. The 0x11 LZSS extension has different
     * token forms and is not accepted by this byte/halfword BIOS service.
     */
    if (compressed_type != 1u ||
        (header & UINT32_C(0xFF)) != UINT32_C(0x10)) {
        return 0;
    }

    /* LZ77UnCompVram can only emit whole halfwords. */
    if (write_halfwords != 0u && (output_size & 1u) != 0u) return 0;
    if (output_size == 0u) return 1;

    const uint64_t destination_end =
        static_cast<uint64_t>(destination) + output_size;
    if (destination_end > GuestAddressEnd ||
        machine->memory_.validate_write(
            cpu, destination, output_size) != memory::Status::Ok) {
        return 0;
    }

    /*
     * Decode into host staging memory first. This avoids partial guest writes
     * for truncated/invalid streams and makes source/destination overlap safe.
     * Input is consumed only as needed; trailing alignment padding is ignored.
     */
    try {
        std::vector<uint8_t> output(output_size, 0u);
        uint64_t cursor = header_end;
        uint32_t produced = 0u;

        const auto read_compressed_byte = [&]() noexcept -> int {
            if (cursor >= GuestAddressEnd) return -1;
            const memory::ReadResult byte = machine->memory_.read(
                cpu, static_cast<uint32_t>(cursor), 1u);
            if (!byte) return -1;
            ++cursor;
            return static_cast<int>(byte.value & UINT32_C(0xFF));
        };

        while (produced < output_size) {
            const int flags_read = read_compressed_byte();
            if (flags_read < 0) return 0;
            uint32_t flags = static_cast<uint32_t>(flags_read);

            for (uint32_t bit = 0u; bit < 8u && produced < output_size; ++bit) {
                if ((flags & UINT32_C(0x80)) == 0u) {
                    const int literal = read_compressed_byte();
                    if (literal < 0) return 0;
                    output[produced++] = static_cast<uint8_t>(literal);
                } else {
                    const int first_read = read_compressed_byte();
                    const int second_read = read_compressed_byte();
                    if (first_read < 0 || second_read < 0) return 0;

                    const uint32_t first =
                        static_cast<uint32_t>(first_read);
                    const uint32_t second =
                        static_cast<uint32_t>(second_read);
                    const uint32_t length = (first >> 4u) + 3u;
                    const uint32_t displacement =
                        (((first & UINT32_C(0x0F)) << 8u) | second) + 1u;
                    if (displacement > produced) return 0;

                    const uint32_t remaining = output_size - produced;
                    const uint32_t copy_length =
                        length < remaining ? length : remaining;
                    for (uint32_t index = 0u; index < copy_length; ++index) {
                        output[produced] = output[produced - displacement];
                        ++produced;
                    }
                }
                flags <<= 1u;
            }
        }

        if (write_halfwords == 0u) {
            return machine->memory_.write_bytes(
                cpu, destination, output.data(), output_size) ==
                memory::Status::Ok ? 1 : 0;
        }

        for (uint32_t offset = 0u; offset < output_size; offset += 2u) {
            const uint32_t value =
                static_cast<uint32_t>(output[offset]) |
                (static_cast<uint32_t>(output[offset + 1u]) << 8u);
            const uint64_t address =
                static_cast<uint64_t>(destination) + offset;
            if (machine->memory_.write(
                    cpu, static_cast<uint32_t>(address), 2u,
                    value) != memory::Status::Ok) {
                return 0;
            }
        }
    } catch (const std::bad_alloc &) {
        return 0;
    }

    return 1;
}


int NdsMachine::runtime_bios_rle_handler(
    void *context,
    NdsCpu cpu,
    uint32_t source,
    uint32_t destination,
    uint32_t write_halfwords)
{
    auto *machine = static_cast<NdsMachine *>(context);
    if (machine == nullptr || !machine->initialized_ ||
        !valid_cpu(cpu) || g_nds_active != cpu ||
        machine->cpu_state(cpu) != oracis_active_cpu_state ||
        write_halfwords > 1u || (source & 3u) != 0u ||
        (write_halfwords != 0u && (destination & 1u) != 0u)) {
        return 0;
    }

    constexpr uint64_t GuestAddressEnd = UINT64_C(0x1'0000'0000);
    const uint64_t header_end =
        static_cast<uint64_t>(source) + UINT64_C(4);
    if (header_end > GuestAddressEnd) return 0;

    const memory::ReadResult header_read =
        machine->memory_.read(cpu, source, 4u);
    if (!header_read) return 0;

    const uint32_t header = header_read.value;
    const uint32_t compressed_type = (header >> 4u) & UINT32_C(0x0F);
    const uint32_t output_size = header >> 8u;
    if (compressed_type != 3u ||
        (header & UINT32_C(0xFF)) != UINT32_C(0x30)) {
        return 0;
    }

    /* RLUnCompVram emits whole halfwords only. */
    if (write_halfwords != 0u && (output_size & 1u) != 0u) return 0;
    if (output_size == 0u) return 1;

    const uint64_t destination_end =
        static_cast<uint64_t>(destination) + output_size;
    if (destination_end > GuestAddressEnd ||
        machine->memory_.validate_write(
            cpu, destination, output_size) != memory::Status::Ok) {
        return 0;
    }

    /*
     * Stage the complete result. A malformed or truncated run must not leave
     * a partially updated guest destination, and in-place decompression must
     * not overwrite unread source bytes.
     */
    try {
        std::vector<uint8_t> output(output_size, 0u);
        uint64_t cursor = header_end;
        uint32_t produced = 0u;

        const auto read_compressed_byte = [&]() noexcept -> int {
            if (cursor >= GuestAddressEnd) return -1;
            const memory::ReadResult byte = machine->memory_.read(
                cpu, static_cast<uint32_t>(cursor), 1u);
            if (!byte) return -1;
            ++cursor;
            return static_cast<int>(byte.value & UINT32_C(0xFF));
        };

        while (produced < output_size) {
            const int flag_read = read_compressed_byte();
            if (flag_read < 0) return 0;

            const uint32_t flag = static_cast<uint32_t>(flag_read);
            const bool compressed = (flag & UINT32_C(0x80)) != 0u;
            const uint32_t run_length =
                (flag & UINT32_C(0x7F)) + (compressed ? 3u : 1u);
            const uint32_t remaining = output_size - produced;
            const uint32_t write_count =
                run_length < remaining ? run_length : remaining;

            if (compressed) {
                const int value_read = read_compressed_byte();
                if (value_read < 0) return 0;
                const uint8_t value = static_cast<uint8_t>(value_read);
                for (uint32_t index = 0u; index < write_count; ++index) {
                    output[produced++] = value;
                }
            } else {
                for (uint32_t index = 0u; index < write_count; ++index) {
                    const int value_read = read_compressed_byte();
                    if (value_read < 0) return 0;
                    output[produced++] = static_cast<uint8_t>(value_read);
                }
            }
        }

        if (write_halfwords == 0u) {
            return machine->memory_.write_bytes(
                cpu, destination, output.data(), output_size) ==
                memory::Status::Ok ? 1 : 0;
        }

        for (uint32_t offset = 0u; offset < output_size; offset += 2u) {
            const uint32_t value =
                static_cast<uint32_t>(output[offset]) |
                (static_cast<uint32_t>(output[offset + 1u]) << 8u);
            const uint64_t address =
                static_cast<uint64_t>(destination) + offset;
            if (machine->memory_.write(
                    cpu, static_cast<uint32_t>(address), 2u,
                    value) != memory::Status::Ok) {
                return 0;
            }
        }
    } catch (const std::bad_alloc &) {
        return 0;
    }

    return 1;
}


int NdsMachine::runtime_bios_huffman_handler(
    void *context,
    NdsCpu cpu,
    uint32_t source,
    uint32_t destination,
    uint32_t callback_structure)
{
    auto *machine = static_cast<NdsMachine *>(context);
    if (machine == nullptr || !machine->initialized_ ||
        !valid_cpu(cpu) || g_nds_active != cpu ||
        machine->cpu_state(cpu) != oracis_active_cpu_state ||
        callback_structure != 0u ||
        (source & 3u) != 0u || (destination & 3u) != 0u) {
        /*
         * NDS BIOS callback streams require invoking guest function pointers
         * for reads. Oracis does not yet bridge guest callbacks, so reject
         * that mode rather than silently bypass the requested data source.
         */
        return 0;
    }

    constexpr uint64_t GuestAddressEnd = UINT64_C(0x1'0000'0000);
    const uint64_t header_end =
        static_cast<uint64_t>(source) + UINT64_C(4);
    if (header_end >= GuestAddressEnd) return 0;

    const memory::ReadResult header_read =
        machine->memory_.read(cpu, source, 4u);
    if (!header_read) return 0;

    const uint32_t header = header_read.value;
    const uint32_t symbol_width = header & UINT32_C(0x0F);
    const uint32_t output_size = header >> 8u;
    if ((header & UINT32_C(0xF0)) != UINT32_C(0x20) ||
        (symbol_width != 4u && symbol_width != 8u) ||
        (output_size & 3u) != 0u) {
        return 0;
    }
    if (output_size == 0u) return 1;

    const uint64_t destination_end =
        static_cast<uint64_t>(destination) + output_size;
    if (destination_end > GuestAddressEnd ||
        machine->memory_.validate_write(
            cpu, destination, output_size) != memory::Status::Ok) {
        return 0;
    }

    const memory::ReadResult tree_size_read = machine->memory_.read(
        cpu, source + 4u, 1u);
    if (!tree_size_read) return 0;

    const uint32_t tree_size = tree_size_read.value & UINT32_C(0xFF);
    const uint32_t tree_bytes = tree_size * 2u + 1u;
    const uint64_t tree_base = static_cast<uint64_t>(source) + 5u;
    const uint64_t tree_end = tree_base + tree_bytes;
    if (tree_end > GuestAddressEnd ||
        machine->memory_.validate_read(
            cpu, static_cast<uint32_t>(tree_base), tree_bytes) !=
            memory::Status::Ok) {
        return 0;
    }

    /*
     * The tree starts at the byte after the size field. Each non-leaf
     * descriptor points to a pair at
     *   (node_address & ~1) + (offset + 1) * 2
     * with the branch selecting the left/right byte. Stage the tree so
     * repeated random-access walks are bounded and independent of writes.
     */
    try {
        std::vector<uint8_t> tree(tree_bytes, 0u);
        for (uint32_t index = 0u; index < tree_bytes; ++index) {
            const memory::ReadResult byte = machine->memory_.read(
                cpu, static_cast<uint32_t>(tree_base + index), 1u);
            if (!byte) return 0;
            tree[index] = static_cast<uint8_t>(byte.value);
        }

        const uint64_t symbol_count =
            (static_cast<uint64_t>(output_size) * 8u) / symbol_width;
        const uint32_t symbols_per_word = 32u / symbol_width;
        const uint32_t word_count = output_size / 4u;
        std::vector<uint32_t> output_words(word_count, 0u);

        uint64_t compressed_cursor = tree_end;
        uint32_t compressed_word = 0u;
        uint32_t compressed_bits_remaining = 0u;

        const auto read_bit = [&]() noexcept -> int {
            if (compressed_bits_remaining == 0u) {
                if (compressed_cursor + 4u > GuestAddressEnd) return -1;
                uint32_t word = 0u;
                for (uint32_t byte_index = 0u; byte_index < 4u; ++byte_index) {
                    const memory::ReadResult byte = machine->memory_.read(
                        cpu,
                        static_cast<uint32_t>(compressed_cursor + byte_index),
                        1u);
                    if (!byte) return -1;
                    word |= (byte.value & UINT32_C(0xFF)) <<
                        (byte_index * 8u);
                }
                compressed_cursor += 4u;
                compressed_word = word;
                compressed_bits_remaining = 32u;
            }

            const uint32_t bit_position = compressed_bits_remaining - 1u;
            const int bit = static_cast<int>(
                (compressed_word >> bit_position) & UINT32_C(1));
            --compressed_bits_remaining;
            return bit;
        };

        const uint32_t symbol_mask =
            symbol_width == 4u ? UINT32_C(0x0F) : UINT32_C(0xFF);
        for (uint64_t symbol_index = 0u;
             symbol_index < symbol_count; ++symbol_index) {
            uint32_t node_index = 0u;
            uint32_t symbol = 0u;
            bool found_leaf = false;

            /*
             * A valid path cannot visit more tree bytes than the tree
             * contains. This bound also rejects cyclic/corrupt descriptors.
             */
            for (uint32_t depth = 0u; depth < tree_bytes; ++depth) {
                if (node_index >= tree_bytes) return 0;
                const uint32_t descriptor = tree[node_index];
                const int branch = read_bit();
                if (branch < 0) return 0;

                const uint64_t node_address = tree_base + node_index;
                const uint64_t child_address =
                    (node_address & ~UINT64_C(1)) +
                    static_cast<uint64_t>(
                        (descriptor & UINT32_C(0x3F)) + 1u) * 2u +
                    static_cast<uint32_t>(branch);
                if (child_address < tree_base || child_address >= tree_end) {
                    return 0;
                }
                const uint32_t child_index =
                    static_cast<uint32_t>(child_address - tree_base);
                const bool child_is_leaf = branch != 0
                    ? (descriptor & UINT32_C(0x40)) != 0u
                    : (descriptor & UINT32_C(0x80)) != 0u;

                if (child_is_leaf) {
                    symbol = tree[child_index];
                    found_leaf = true;
                    break;
                }
                node_index = child_index;
            }

            if (!found_leaf || (symbol & ~symbol_mask) != 0u) return 0;
            const uint32_t word_index =
                static_cast<uint32_t>(symbol_index / symbols_per_word);
            const uint32_t bit_shift =
                static_cast<uint32_t>(symbol_index % symbols_per_word) *
                symbol_width;
            output_words[word_index] |= symbol << bit_shift;
        }

        /*
         * BIOS Huffman stores whole 32-bit words. A multiple-of-four output
         * size means every symbol belongs to a complete staged destination
         * word; no implicit overrun or uninitialized padding is permitted.
         */
        for (uint32_t index = 0u; index < word_count; ++index) {
            const uint64_t address =
                static_cast<uint64_t>(destination) +
                static_cast<uint64_t>(index) * 4u;
            if (machine->memory_.write(
                    cpu, static_cast<uint32_t>(address), 4u,
                    output_words[index]) != memory::Status::Ok) {
                return 0;
            }
        }
    } catch (const std::bad_alloc &) {
        return 0;
    }

    return 1;
}


int NdsMachine::runtime_bios_diff_handler(
    void *context,
    NdsCpu cpu,
    uint32_t source,
    uint32_t destination,
    uint32_t unit_width)
{
    auto *machine = static_cast<NdsMachine *>(context);
    if (machine == nullptr || !machine->initialized_ ||
        cpu != NDS_ARM9 || g_nds_active != cpu ||
        machine->cpu_state(cpu) != oracis_active_cpu_state ||
        (unit_width != 1u && unit_width != 2u) ||
        (source & 3u) != 0u ||
        (unit_width == 2u && (destination & 1u) != 0u)) {
        return 0;
    }

    constexpr uint64_t GuestAddressEnd = UINT64_C(0x1'0000'0000);
    const uint64_t header_end =
        static_cast<uint64_t>(source) + UINT64_C(4);
    if (header_end >= GuestAddressEnd) return 0;

    const memory::ReadResult header_read =
        machine->memory_.read(cpu, source, 4u);
    if (!header_read) return 0;

    const uint32_t header = header_read.value;
    const uint32_t expected_unit_code = unit_width == 1u ? 1u : 2u;
    const uint32_t output_size = header >> 8u;
    if ((header & UINT32_C(0xF0)) != UINT32_C(0x80) ||
        (header & UINT32_C(0x0F)) != expected_unit_code ||
        (output_size % unit_width) != 0u) {
        return 0;
    }
    if (output_size == 0u) return 1;

    const uint64_t source_end = header_end + output_size;
    const uint64_t destination_end =
        static_cast<uint64_t>(destination) + output_size;
    if (source_end > GuestAddressEnd ||
        destination_end > GuestAddressEnd ||
        machine->memory_.validate_read(
            cpu, static_cast<uint32_t>(header_end), output_size) !=
            memory::Status::Ok ||
        machine->memory_.validate_write(
            cpu, destination, output_size) != memory::Status::Ok) {
        return 0;
    }

    /*
     * The first unit is the original sample. Each later unit is an unsigned
     * delta added modulo the sample width. Stage the entire stream so source
     * and destination can overlap without corrupting unread deltas.
     */
    try {
        if (unit_width == 1u) {
            std::vector<uint8_t> output(output_size, 0u);
            uint32_t accumulator = 0u;
            for (uint32_t index = 0u; index < output_size; ++index) {
                const memory::ReadResult encoded = machine->memory_.read(
                    cpu, static_cast<uint32_t>(header_end + index), 1u);
                if (!encoded) return 0;
                accumulator = index == 0u
                    ? (encoded.value & UINT32_C(0xFF))
                    : ((accumulator + encoded.value) & UINT32_C(0xFF));
                output[index] = static_cast<uint8_t>(accumulator);
            }
            return machine->memory_.write_bytes(
                cpu, destination, output.data(), output_size) ==
                memory::Status::Ok ? 1 : 0;
        }

        std::vector<uint16_t> output(output_size / 2u, 0u);
        uint32_t accumulator = 0u;
        for (uint32_t index = 0u; index < output.size(); ++index) {
            const uint64_t address = header_end +
                static_cast<uint64_t>(index) * 2u;
            const memory::ReadResult encoded = machine->memory_.read(
                cpu, static_cast<uint32_t>(address), 2u);
            if (!encoded) return 0;
            accumulator = index == 0u
                ? (encoded.value & UINT32_C(0xFFFF))
                : ((accumulator + encoded.value) & UINT32_C(0xFFFF));
            output[index] = static_cast<uint16_t>(accumulator);
        }

        for (uint32_t index = 0u; index < output.size(); ++index) {
            const uint64_t address =
                static_cast<uint64_t>(destination) +
                static_cast<uint64_t>(index) * 2u;
            if (machine->memory_.write(
                    cpu, static_cast<uint32_t>(address), 2u,
                    output[index]) != memory::Status::Ok) {
                return 0;
            }
        }
    } catch (const std::bad_alloc &) {
        return 0;
    }

    return 1;
}


int NdsMachine::check_arm9_protection_access(
    NdsCpu cpu, uint32_t address, uint32_t size,
    uint32_t access_kind, bool privileged) const noexcept
{
    if (!valid_cpu(cpu) || size == 0u) return -1;
    if (cpu == NDS_ARM7 || !arm9_protection_unit_.enabled()) return 1;

    memory::NdsArm9ProtectionUnit::AccessKind kind{};
    switch (access_kind) {
    case ORACIS_BUS_ACCESS_DATA_READ:
        kind = memory::NdsArm9ProtectionUnit::AccessKind::DataRead;
        break;
    case ORACIS_BUS_ACCESS_DATA_WRITE:
        kind = memory::NdsArm9ProtectionUnit::AccessKind::DataWrite;
        break;
    case ORACIS_BUS_ACCESS_INSTRUCTION:
        kind = memory::NdsArm9ProtectionUnit::AccessKind::InstructionFetch;
        break;
    default:
        return -1;
    }

    const memory::NdsArm9Tcm::TimingState tcm =
        arm9_tcm_.timing_state();
    const uint64_t end =
        static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    if (end > UINT64_C(0x1'0000'0000)) return -1;

    uint64_t current = address;
    while (current < end) {
        const uint32_t guest_address = static_cast<uint32_t>(current);
        const bool in_itcm = tcm.itcm_window_size != 0u &&
            current < tcm.itcm_window_size;
        const bool in_dtcm = tcm.dtcm_window_size != 0u &&
            current >= tcm.dtcm_base &&
            current < static_cast<uint64_t>(tcm.dtcm_base) +
                tcm.dtcm_window_size;
        if (in_itcm || in_dtcm) {
            const uint64_t tcm_end = in_itcm
                ? tcm.itcm_window_size
                : static_cast<uint64_t>(tcm.dtcm_base) +
                    tcm.dtcm_window_size;
            current = tcm_end < end ? tcm_end : end;
            continue;
        }
        if (!arm9_protection_unit_.check_access(
                guest_address, 1u, kind, privileged)) {
            return 0;
        }
        ++current;
    }
    return 1;
}

int NdsMachine::runtime_bus_access_check(
    void *context, NdsCpu cpu, uint32_t address, uint32_t size,
    uint32_t access_kind, uint32_t privileged)
{
    auto *machine = static_cast<NdsMachine *>(context);
    if (machine == nullptr || !machine->initialized_ ||
        !valid_cpu(cpu) || size == 0u || privileged > 1u ||
        g_nds_active != cpu ||
        machine->cpu_state(cpu) != oracis_active_cpu_state) {
        return -1;
    }
    if (machine->arm9_data_abort_pending_) return 0;

    const int allowed = machine->check_arm9_protection_access(
        cpu, address, size, access_kind, privileged != 0u);
    if (allowed != 0) return allowed;

    /* One fault snapshot per instruction; later accesses in the same
       generated C block are suppressed until runtime_tick enters Abort. */
    if (!machine->arm9_data_abort_pending_) {
        machine->arm9_data_abort_pending_ = true;
        machine->arm9_data_abort_snapshot_ = *oracis_active_cpu_state;
        machine->arm9_data_abort_address_ = address;
        machine->arm9_data_abort_size_ = size;
        machine->arm9_data_abort_access_kind_ = access_kind;
    }
    return 0;
}

int NdsMachine::runtime_data_abort_handler(void *context, NdsCpu cpu)
{
    auto *machine = static_cast<NdsMachine *>(context);
    if (machine == nullptr || !machine->initialized_ ||
        (cpu != NDS_ARM9 && cpu != NDS_ARM7) ||
        g_nds_active != cpu ||
        machine->cpu_state(cpu) != oracis_active_cpu_state) {
        return -1;
    }
    /* Data Abort is wired only for ARM9; ARM7 has its own separate core model. */
    if (cpu == NDS_ARM7) {
        return machine->arm9_data_abort_pending_ ? -1 : 0;
    }
    if (!machine->arm9_data_abort_pending_) return 0;

    ArmCpuState *state = machine->cpu_state(NDS_ARM9);
    if (state == nullptr) return -1;

    const uint32_t fault_pc = machine->arm9_data_abort_snapshot_.R[15];
    const uint64_t return_address =
        static_cast<uint64_t>(fault_pc) + UINT64_C(8);
    if (return_address > UINT32_MAX) return -1;

    *state = machine->arm9_data_abort_snapshot_;
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        !oracis_arm_cpu_enter_exception(
            state, ORACIS_ARM_EXCEPTION_DATA_ABORT,
            static_cast<uint32_t>(return_address),
            machine->exception_vector_base(NDS_ARM9))) {
        return -1;
    }

    std::fprintf(stderr,
        "[Oracis ARM9] Data Abort: PC=0x%08X address=0x%08X "
        "size=%u access=%s vector=0x%08X\n",
        fault_pc,
        machine->arm9_data_abort_address_,
        machine->arm9_data_abort_size_,
        machine->arm9_data_abort_access_kind_ ==
                ORACIS_BUS_ACCESS_DATA_WRITE ? "write" : "read",
        state->R[15]);

    machine->arm9_data_abort_pending_ = false;
    machine->arm9_data_abort_address_ = 0u;
    machine->arm9_data_abort_size_ = 0u;
    machine->arm9_data_abort_access_kind_ = 0u;
    return 1;
}

int NdsMachine::runtime_prefetch_abort_handler(
    void *context, NdsCpu cpu, uint32_t target_pc)
{
    auto *machine = static_cast<NdsMachine *>(context);
    if (machine == nullptr || !machine->initialized_ ||
        (cpu != NDS_ARM9 && cpu != NDS_ARM7) ||
        g_nds_active != cpu ||
        machine->cpu_state(cpu) != oracis_active_cpu_state) {
        return -1;
    }

    /* ARM7 has no ARM9 MPU; its fetch faults remain outside this handler. */
    if (cpu == NDS_ARM7) return 0;

    ArmCpuState *state = machine->cpu_state(NDS_ARM9);
    if (state == nullptr) return -1;

    const bool thumb = (state->cpsr & CPSR_T_BIT) != 0u;
    const uint32_t fault_pc = target_pc & ~UINT32_C(1);
    const uint32_t instruction_size = thumb ? 2u : 4u;
    const uint32_t alignment = thumb ? 2u : 4u;
    if ((fault_pc % alignment) != 0u) return 0;

    /*
     * A dispatch miss is not itself a CPU abort. First require a backing
     * region that GuestMemory permits for instruction fetch, then test the
     * ARM9 instruction MPU. Unmapped and non-executable mappings remain
     * diagnostic dispatch failures.
     */
    if (machine->memory_.validate_execute(
            NDS_ARM9, fault_pc, instruction_size) != memory::Status::Ok) {
        return 0;
    }

    const uint32_t mode = state->cpsr & UINT32_C(0x1F);
    bool privileged = false;
    switch (mode) {
    case UINT32_C(0x10): /* User */
        privileged = false;
        break;
    case UINT32_C(0x11): /* FIQ */
    case UINT32_C(0x12): /* IRQ */
    case UINT32_C(0x13): /* Supervisor */
    case UINT32_C(0x17): /* Abort */
    case UINT32_C(0x1B): /* Undefined */
    case UINT32_C(0x1F): /* System */
        privileged = true;
        break;
    default:
        return -1;
    }

    if (machine->check_arm9_protection_access(
            NDS_ARM9, fault_pc, instruction_size,
            ORACIS_BUS_ACCESS_INSTRUCTION, privileged) != 0) {
        return 0;
    }

    const uint64_t return_address =
        static_cast<uint64_t>(fault_pc) + UINT64_C(4);
    if (return_address > UINT32_MAX) return -1;

    state->R[15] = fault_pc;
    if (!oracis_arm_cpu_enter_exception(
            state, ORACIS_ARM_EXCEPTION_PREFETCH_ABORT,
            static_cast<uint32_t>(return_address),
            machine->exception_vector_base(NDS_ARM9))) {
        return -1;
    }

    std::fprintf(stderr,
        "[Oracis ARM9] Prefetch Abort: PC=0x%08X vector=0x%08X\\n",
        fault_pc, state->R[15]);
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

    if (cpu == NDS_ARM9) {
        const uint32_t mode =
            machine->cpu_state(cpu)->cpsr & UINT32_C(0x1F);
        uint32_t privileged = 2u;
        if (mode == UINT32_C(0x10)) {
            privileged = 0u;
        } else if (mode == UINT32_C(0x11) ||
                   mode == UINT32_C(0x12) ||
                   mode == UINT32_C(0x13) ||
                   mode == UINT32_C(0x17) ||
                   mode == UINT32_C(0x1B) ||
                   mode == UINT32_C(0x1F)) {
            privileged = 1u;
        }
        if (privileged > 1u ||
            machine->check_arm9_protection_access(
                cpu, validation->addr, validation->size,
                ORACIS_BUS_ACCESS_INSTRUCTION,
                privileged != 0u) != 1) {
            return 0;
        }
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
    if (initialized_ || wram_control_.installed() ||
        vram_control_.installed() || video_memory_.installed() ||
        post_flag_.installed() || display_control_.installed() ||
        geometry_engine_.installed() || game_card_control_.installed()) {
        return NdsMachineStatus::AlreadyInitialized;
    }

    arm9_state_ = {};
    arm7_state_ = {};
    arm9_data_abort_pending_ = false;
    arm9_data_abort_snapshot_ = {};
    arm9_data_abort_address_ = 0u;
    arm9_data_abort_size_ = 0u;
    arm9_data_abort_access_kind_ = 0u;
    interrupt_wait_active_.fill(false);
    interrupt_wait_masks_.fill(0u);
    interrupt_wait_resume_pcs_.fill(0u);
    program_loaded_ = false;
    /* CP15 Control.V and fixed-one bits are owned by the ARM9 TCM controller. */
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
    oracis_runtime_set_arm9_tcm_timing_state(0u, 0u, 0u, 0u, 0u);

    const auto rollback = [this](NdsMachineStatus cause) noexcept {
        return shutdown() == NdsMachineStatus::Ok
            ? cause : NdsMachineStatus::ShutdownFailed;
    };

    if (!oracis_arm_runtime_bind_power_handler(
            this, &NdsMachine::runtime_power_handler)) {
        return rollback(NdsMachineStatus::RuntimeAlreadyBound);
    }
    power_handler_bound_ = true;

    if (!oracis_arm_runtime_bind_irq_handler(
            this, &NdsMachine::runtime_irq_handler)) {
        return rollback(NdsMachineStatus::RuntimeAlreadyBound);
    }
    irq_handler_bound_ = true;

    if (!oracis_arm_runtime_bind_data_abort_handler(
            this, &NdsMachine::runtime_data_abort_handler)) {
        return rollback(NdsMachineStatus::RuntimeAlreadyBound);
    }
    data_abort_handler_bound_ = true;

    if (!oracis_arm_runtime_bind_prefetch_abort_handler(
            this, &NdsMachine::runtime_prefetch_abort_handler)) {
        return rollback(NdsMachineStatus::RuntimeAlreadyBound);
    }
    prefetch_abort_handler_bound_ = true;

    if (!oracis_arm_runtime_bind_interrupt_wait_handler(
            this, &NdsMachine::runtime_interrupt_wait_handler)) {
        return rollback(NdsMachineStatus::RuntimeAlreadyBound);
    }
    interrupt_wait_handler_bound_ = true;

    if (!oracis_arm_runtime_bind_coprocessor_handler(
            this, &NdsMachine::runtime_coprocessor_handler)) {
        return rollback(NdsMachineStatus::RuntimeAlreadyBound);
    }
    coprocessor_handler_bound_ = true;

    if (!oracis_arm_runtime_bind_bios_memory_handler(
            this, &NdsMachine::runtime_bios_memory_transfer_handler)) {
        return rollback(NdsMachineStatus::RuntimeAlreadyBound);
    }
    bios_memory_handler_bound_ = true;

    if (!oracis_arm_runtime_bind_bios_crc16_handler(
            this, &NdsMachine::runtime_bios_crc16_handler)) {
        return rollback(NdsMachineStatus::RuntimeAlreadyBound);
    }
    bios_crc16_handler_bound_ = true;

    if (!oracis_arm_runtime_bind_bios_bit_unpack_handler(
            this, &NdsMachine::runtime_bios_bit_unpack_handler)) {
        return rollback(NdsMachineStatus::RuntimeAlreadyBound);
    }
    bios_bit_unpack_handler_bound_ = true;

    if (!oracis_arm_runtime_bind_bios_lz77_handler(
            this, &NdsMachine::runtime_bios_lz77_handler)) {
        return rollback(NdsMachineStatus::RuntimeAlreadyBound);
    }
    bios_lz77_handler_bound_ = true;

    if (!oracis_arm_runtime_bind_bios_rle_handler(
            this, &NdsMachine::runtime_bios_rle_handler)) {
        return rollback(NdsMachineStatus::RuntimeAlreadyBound);
    }
    bios_rle_handler_bound_ = true;

    if (!oracis_arm_runtime_bind_bios_huffman_handler(
            this, &NdsMachine::runtime_bios_huffman_handler)) {
        return rollback(NdsMachineStatus::RuntimeAlreadyBound);
    }
    bios_huffman_handler_bound_ = true;

    if (!oracis_arm_runtime_bind_bios_diff_handler(
            this, &NdsMachine::runtime_bios_diff_handler)) {
        return rollback(NdsMachineStatus::RuntimeAlreadyBound);
    }
    bios_diff_handler_bound_ = true;

    if (wram_control_.install() != memory::Status::Ok ||
        vram_control_.install() != memory::Status::Ok ||
        video_memory_.install() != memory::Status::Ok ||
        post_flag_.install() != memory::Status::Ok ||
        display_control_.install() != memory::Status::Ok ||
        geometry_engine_.install() != memory::Status::Ok ||
        game_card_control_.install() != memory::Status::Ok ||
        arm9_tcm_.install() != memory::Status::Ok) {
        return rollback(NdsMachineStatus::MemoryMapFailed);
    }
    sync_arm9_tcm_timing_state();

    if (!oracis_bus_bind_memory(&memory_)) {
        return rollback(NdsMachineStatus::BusBindingFailed);
    }
    memory_bound_ = true;

    if (!oracis_bus_bind_arm9_access_check(
            this, &NdsMachine::runtime_bus_access_check)) {
        return rollback(NdsMachineStatus::BusBindingFailed);
    }
    arm9_access_guard_bound_ = true;

    if (!oracis_bus_bind_post_flag(&post_flag_)) {
        return rollback(NdsMachineStatus::BusBindingFailed);
    }
    post_flag_bound_ = true;

    if (!oracis_bus_bind_wram_control(&wram_control_)) {
        return rollback(NdsMachineStatus::BusBindingFailed);
    }
    wram_bound_ = true;

    if (!oracis_bus_bind_vram_control(&vram_control_)) {
        return rollback(NdsMachineStatus::BusBindingFailed);
    }
    vram_bound_ = true;

    if (!oracis_bus_bind_video_memory(&video_memory_)) {
        return rollback(NdsMachineStatus::BusBindingFailed);
    }
    video_memory_bound_ = true;


    if (!oracis_bus_bind_interrupt_control(&interrupt_control_)) {
        return rollback(NdsMachineStatus::BusBindingFailed);
    }
    interrupt_bound_ = true;

    if (!oracis_bus_bind_game_card_control(&game_card_control_)) {
        return rollback(NdsMachineStatus::BusBindingFailed);
    }
    game_card_bound_ = true;
    if (!oracis_bus_bind_ipc_control(&ipc_control_)) {
        return rollback(NdsMachineStatus::BusBindingFailed);
    }
    ipc_bound_ = true;

    if (!oracis_bus_bind_dma_control(&dma_control_)) {
        return rollback(NdsMachineStatus::BusBindingFailed);
    }
    dma_bound_ = true;

    if (!oracis_bus_bind_display_control(&display_control_)) {
        return rollback(NdsMachineStatus::BusBindingFailed);
    }
    display_control_bound_ = true;

    if (!oracis_bus_bind_geometry_engine(&geometry_engine_)) {
        return rollback(NdsMachineStatus::BusBindingFailed);
    }
    geometry_engine_bound_ = true;

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
    const NdsProgramSegment &arm7,
    const uint8_t *cartridge_header,
    uint32_t cartridge_header_size) noexcept
{
    if (!initialized_) return NdsMachineStatus::NotInitialized;

    const bool has_cartridge_header = cartridge_header != nullptr;
    if ((has_cartridge_header &&
         cartridge_header_size != memory::NdsRetailRamMap::CartridgeHeaderStoredSize) ||
        (!has_cartridge_header && cartridge_header_size != 0u)) {
        return NdsMachineStatus::InvalidProgramImage;
    }
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

    if (has_cartridge_header &&
        memory_.validate_write(
            NDS_ARM9,
            memory::NdsRetailRamMap::CartridgeHeaderRamAddress,
            memory::NdsRetailRamMap::CartridgeHeaderStoredSize) !=
            memory::Status::Ok) {
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
            memory::Status::Ok ||
        (has_cartridge_header &&
         memory_.write_bytes(
             NDS_ARM9,
             memory::NdsRetailRamMap::CartridgeHeaderRamAddress,
             cartridge_header, cartridge_header_size) !=
             memory::Status::Ok)) {
        return NdsMachineStatus::ProgramLoadFailed;
    }

    /* Confirm that both program images and the optional boot header are live. */
    if (memory_.validate_identity(
            NDS_ARM9, arm9.load_address, arm9.size, arm9.contents) !=
            memory::Status::Ok ||
        memory_.validate_identity(
            NDS_ARM7, arm7.load_address, arm7.size, arm7.contents) !=
            memory::Status::Ok ||
        (has_cartridge_header &&
         memory_.validate_identity(
             NDS_ARM9,
             memory::NdsRetailRamMap::CartridgeHeaderRamAddress,
             cartridge_header_size, cartridge_header) !=
             memory::Status::Ok)) {
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
    interrupt_wait_active_.fill(false);
    interrupt_wait_masks_.fill(0u);
    interrupt_wait_resume_pcs_.fill(0u);
    program_loaded_ = true;
    return NdsMachineStatus::Ok;
}

NdsMachineStatus NdsMachine::shutdown() noexcept
{
    const bool owns_runtime =
        runtime_bound_ || power_handler_bound_ || irq_handler_bound_ ||
        data_abort_handler_bound_ || prefetch_abort_handler_bound_ ||
        interrupt_wait_handler_bound_ ||
        coprocessor_handler_bound_ || bios_memory_handler_bound_ ||
        bios_crc16_handler_bound_ || bios_bit_unpack_handler_bound_ ||
        bios_lz77_handler_bound_ || bios_rle_handler_bound_ ||
        bios_huffman_handler_bound_ || bios_diff_handler_bound_ ||
        memory_bound_ || arm9_access_guard_bound_ ||
        wram_bound_ || vram_bound_ || video_memory_bound_ ||
        post_flag_bound_ || display_control_bound_ ||
        geometry_engine_bound_ || interrupt_bound_ ||
        game_card_bound_ || ipc_bound_ || dma_bound_ || timer_bound_ ||
        keypad_bound_ || clock_bound_ || validation_bound_;
    if (!owns_runtime && !initialized_) {
        return NdsMachineStatus::Ok;
    }

    arm9_data_abort_pending_ = false;
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
    if (vram_bound_) {
        if (!oracis_bus_unbind_vram_control(&vram_control_)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        vram_bound_ = false;
    }
    if (display_control_bound_) {
        if (!oracis_bus_unbind_display_control(&display_control_)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        display_control_bound_ = false;
    }
    if (geometry_engine_bound_) {
        if (!oracis_bus_unbind_geometry_engine(&geometry_engine_)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        geometry_engine_bound_ = false;
    }
    if (dma_bound_) {
        if (!oracis_bus_unbind_dma_control(&dma_control_)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        dma_bound_ = false;
    }
    if (game_card_bound_) {
        if (!oracis_bus_unbind_game_card_control(&game_card_control_)) return NdsMachineStatus::ShutdownFailed;
        game_card_bound_ = false;
    }
    if (ipc_bound_) {
        if (!oracis_bus_unbind_ipc_control(&ipc_control_)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        ipc_bound_ = false;
    }
    if (interrupt_bound_) {
        if (!oracis_bus_unbind_interrupt_control(&interrupt_control_)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        interrupt_bound_ = false;
    }
    if (video_memory_bound_) {
        if (!oracis_bus_unbind_video_memory(&video_memory_)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        video_memory_bound_ = false;
    }
    if (post_flag_bound_) {
        if (!oracis_bus_unbind_post_flag(&post_flag_)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        post_flag_bound_ = false;
    }
    if (wram_bound_) {
        if (!oracis_bus_unbind_wram_control(&wram_control_)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        wram_bound_ = false;
    }
    if (arm9_access_guard_bound_) {
        if (!oracis_bus_unbind_arm9_access_check(
                this, &NdsMachine::runtime_bus_access_check)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        arm9_access_guard_bound_ = false;
    }
    if (memory_bound_) {
        if (!oracis_bus_unbind_memory(&memory_)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        memory_bound_ = false;
    }
    if (bios_diff_handler_bound_) {
        if (!oracis_arm_runtime_unbind_bios_diff_handler(
                this, &NdsMachine::runtime_bios_diff_handler)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        bios_diff_handler_bound_ = false;
    }
    if (bios_huffman_handler_bound_) {
        if (!oracis_arm_runtime_unbind_bios_huffman_handler(
                this, &NdsMachine::runtime_bios_huffman_handler)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        bios_huffman_handler_bound_ = false;
    }
    if (bios_rle_handler_bound_) {
        if (!oracis_arm_runtime_unbind_bios_rle_handler(
                this, &NdsMachine::runtime_bios_rle_handler)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        bios_rle_handler_bound_ = false;
    }
    if (bios_lz77_handler_bound_) {
        if (!oracis_arm_runtime_unbind_bios_lz77_handler(
                this, &NdsMachine::runtime_bios_lz77_handler)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        bios_lz77_handler_bound_ = false;
    }
    if (bios_bit_unpack_handler_bound_) {
        if (!oracis_arm_runtime_unbind_bios_bit_unpack_handler(
                this, &NdsMachine::runtime_bios_bit_unpack_handler)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        bios_bit_unpack_handler_bound_ = false;
    }
    if (bios_crc16_handler_bound_) {
        if (!oracis_arm_runtime_unbind_bios_crc16_handler(
                this, &NdsMachine::runtime_bios_crc16_handler)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        bios_crc16_handler_bound_ = false;
    }
    if (bios_memory_handler_bound_) {
        if (!oracis_arm_runtime_unbind_bios_memory_handler(
                this, &NdsMachine::runtime_bios_memory_transfer_handler)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        bios_memory_handler_bound_ = false;
    }
    if (coprocessor_handler_bound_) {
        if (!oracis_arm_runtime_unbind_coprocessor_handler(
                this, &NdsMachine::runtime_coprocessor_handler)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        coprocessor_handler_bound_ = false;
    }
    if (interrupt_wait_handler_bound_) {
        if (!oracis_arm_runtime_unbind_interrupt_wait_handler(
                this, &NdsMachine::runtime_interrupt_wait_handler)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        interrupt_wait_handler_bound_ = false;
    }
    if (prefetch_abort_handler_bound_) {
        if (!oracis_arm_runtime_unbind_prefetch_abort_handler(
                this, &NdsMachine::runtime_prefetch_abort_handler)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        prefetch_abort_handler_bound_ = false;
    }
    if (data_abort_handler_bound_) {
        if (!oracis_arm_runtime_unbind_data_abort_handler(
                this, &NdsMachine::runtime_data_abort_handler)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        data_abort_handler_bound_ = false;
    }
    if (irq_handler_bound_) {
        if (!oracis_arm_runtime_unbind_irq_handler(
                this, &NdsMachine::runtime_irq_handler)) {
            return NdsMachineStatus::ShutdownFailed;
        }
        irq_handler_bound_ = false;
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
    oracis_runtime_set_arm9_tcm_timing_state(0u, 0u, 0u, 0u, 0u);

    oracis_scheduler_reset(NDS_ARM9);
    oracis_scheduler_reset(NDS_ARM7);
    system_clock_.reset();
    run_states_.fill(NdsCpuRunState::Running);
    interrupt_wait_active_.fill(false);
    interrupt_wait_masks_.fill(0u);
    interrupt_wait_resume_pcs_.fill(0u);
    idle_core_cycles_offset_.fill(0u);
    next_round_first_cpu_ = NDS_ARM9;
    program_loaded_ = false;
    three_d_scanline_sequence_initialized_ = false;
    next_three_d_producer_scanline_ = 0u;
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
            /*
             * Native Oracis banks keep R15 at the next instruction boundary.
             * The runtime IRQ contract converts this PC to LR_irq = PC + 4.
             * This enters the vector between slices; the next slice dispatches
             * the compiled handler. Custom executors can still provide an
             * instruction-specific link through irq_context below.
             */
            if (!oracis_arm_runtime_select_cpu(cpu)) {
                return NdsMachineStatus::SchedulerFailure;
            }
            const int irq_result =
                oracis_arm_runtime_request_irq(state->R[15]);
            return irq_result == 1
                ? NdsMachineStatus::Ok
                : (irq_result == 0
                    ? NdsMachineStatus::IrqContextUnavailable
                    : NdsMachineStatus::IrqEntryFailed);
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
        (void)legacy_vector_base; /* Retained only for callback ABI compatibility. */

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

NdsMachineStatus NdsMachine::read_bios_interrupt_flags(
    NdsCpu cpu, uint32_t &flags) const noexcept
{
    if (!valid_cpu(cpu)) return NdsMachineStatus::InvalidCpu;

    uint32_t address = UINT32_C(0x0380FFF8);
    if (cpu == NDS_ARM9) {
        constexpr uint32_t DtcmInterruptFlagsOffset = UINT32_C(0x00003FF8);
        const memory::NdsArm9Tcm::TimingState timing =
            arm9_tcm_.timing_state();
        if (timing.dtcm_base == 0u ||
            timing.dtcm_window_size <
                memory::NdsArm9Tcm::DtcmPhysicalSize ||
            timing.dtcm_base >
                UINT32_MAX - DtcmInterruptFlagsOffset) {
            return NdsMachineStatus::UnsupportedPowerMode;
        }
        address = timing.dtcm_base + DtcmInterruptFlagsOffset;
    }

    const memory::ReadResult result = memory_.read(cpu, address, 4u);
    if (result.status != memory::Status::Ok) {
        return NdsMachineStatus::SchedulerFailure;
    }
    flags = result.value;
    return NdsMachineStatus::Ok;
}

NdsMachineStatus NdsMachine::write_bios_interrupt_flags(
    NdsCpu cpu, uint32_t flags) noexcept
{
    if (!valid_cpu(cpu)) return NdsMachineStatus::InvalidCpu;

    uint32_t address = UINT32_C(0x0380FFF8);
    if (cpu == NDS_ARM9) {
        constexpr uint32_t DtcmInterruptFlagsOffset = UINT32_C(0x00003FF8);
        const memory::NdsArm9Tcm::TimingState timing =
            arm9_tcm_.timing_state();
        if (timing.dtcm_base == 0u ||
            timing.dtcm_window_size <
                memory::NdsArm9Tcm::DtcmPhysicalSize ||
            timing.dtcm_base >
                UINT32_MAX - DtcmInterruptFlagsOffset) {
            return NdsMachineStatus::UnsupportedPowerMode;
        }
        address = timing.dtcm_base + DtcmInterruptFlagsOffset;
    }

    return memory_.write(cpu, address, 4u, flags) == memory::Status::Ok
        ? NdsMachineStatus::Ok
        : NdsMachineStatus::SchedulerFailure;
}

NdsMachineStatus NdsMachine::begin_interrupt_wait(
    NdsCpu cpu, uint32_t clear_old_flags, uint32_t wait_mask) noexcept
{
    if (!initialized_) return NdsMachineStatus::NotInitialized;
    if (!valid_cpu(cpu)) return NdsMachineStatus::InvalidCpu;
    const unsigned index = cpu_index(cpu);
    if (slice_active_[index ^ 1u]) {
        return NdsMachineStatus::SliceAlreadyActive;
    }
    ArmCpuState *state = cpu_state(cpu);
    if (state == nullptr) return NdsMachineStatus::InvalidCpu;

    /*
     * Match the BIOS critical section: mask IME while checking/consuming its
     * per-CPU IRQ shadow word, then force IME back on before HALT.
     */
    if (interrupt_control_.write(
            cpu, memory::NdsInterruptControl::ImeAddress,
            4u, 0u) != memory::Status::Ok) {
        return NdsMachineStatus::SchedulerFailure;
    }

    uint32_t flags = 0u;
    NdsMachineStatus flag_status = read_bios_interrupt_flags(cpu, flags);
    if (flag_status != NdsMachineStatus::Ok) {
        static_cast<void>(interrupt_control_.write(
            cpu, memory::NdsInterruptControl::ImeAddress, 4u, 1u));
        return flag_status;
    }

    const uint32_t matched_flags = flags & wait_mask;
    if (clear_old_flags != 0u || matched_flags != 0u) {
        flag_status = write_bios_interrupt_flags(
            cpu, flags & ~matched_flags);
        if (flag_status != NdsMachineStatus::Ok) {
            static_cast<void>(interrupt_control_.write(
                cpu, memory::NdsInterruptControl::ImeAddress, 4u, 1u));
            return flag_status;
        }
    }

    /* KEEP mode returns immediately if a BIOS IRQ-check flag is already set. */
    if (clear_old_flags == 0u && matched_flags != 0u) {
        interrupt_wait_active_[index] = false;
        interrupt_wait_masks_[index] = 0u;
        interrupt_wait_resume_pcs_[index] = 0u;
        run_states_[index] = NdsCpuRunState::Running;
        return interrupt_control_.write(
            cpu, memory::NdsInterruptControl::ImeAddress,
            4u, 1u) == memory::Status::Ok
            ? NdsMachineStatus::Ok : NdsMachineStatus::SchedulerFailure;
    }

    if (interrupt_control_.write(
            cpu, memory::NdsInterruptControl::ImeAddress,
            4u, 1u) != memory::Status::Ok) {
        return NdsMachineStatus::SchedulerFailure;
    }

    interrupt_wait_active_[index] = true;
    interrupt_wait_masks_[index] = wait_mask;
    interrupt_wait_resume_pcs_[index] = state->R[15];
    run_states_[index] = NdsCpuRunState::Halted;

    /* A BIOS wait called from a generated bank must unwind at this boundary. */
    if (slice_active_[index] && !oracis_scheduler_request_yield(cpu)) {
        interrupt_wait_active_[index] = false;
        interrupt_wait_masks_[index] = 0u;
        interrupt_wait_resume_pcs_[index] = 0u;
        run_states_[index] = NdsCpuRunState::Running;
        return NdsMachineStatus::SchedulerFailure;
    }
    return NdsMachineStatus::Ok;
}

NdsMachineStatus NdsMachine::resume_interrupt_wait(
    NdsCpu cpu, uint32_t return_pc) noexcept
{
    if (!initialized_) return NdsMachineStatus::NotInitialized;
    if (!valid_cpu(cpu)) return NdsMachineStatus::InvalidCpu;
    const unsigned index = cpu_index(cpu);
    if (!interrupt_wait_active_[index] ||
        interrupt_wait_resume_pcs_[index] != return_pc) {
        return NdsMachineStatus::Ok;
    }

    uint32_t flags = 0u;
    const NdsMachineStatus flag_status =
        read_bios_interrupt_flags(cpu, flags);
    if (flag_status != NdsMachineStatus::Ok) return flag_status;

    const uint32_t matched_flags = flags & interrupt_wait_masks_[index];
    if (matched_flags != 0u) {
        const NdsMachineStatus write_status = write_bios_interrupt_flags(
            cpu, flags & ~matched_flags);
        if (write_status != NdsMachineStatus::Ok) return write_status;
        interrupt_wait_active_[index] = false;
        interrupt_wait_masks_[index] = 0u;
        interrupt_wait_resume_pcs_[index] = 0u;
        run_states_[index] = NdsCpuRunState::Running;
        return NdsMachineStatus::Ok;
    }

    /* An IRQ handler without a matching BIOS shadow flag returned: wait again. */
    run_states_[index] = NdsCpuRunState::Halted;
    if (slice_active_[index] && !oracis_scheduler_request_yield(cpu)) {
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

bool NdsMachine::cpu_slice_active(NdsCpu cpu) const noexcept
{
    return initialized_ && valid_cpu(cpu) && slice_active_[cpu_index(cpu)];
}

NdsMachineStatus NdsMachine::enter_runtime_irq(
    NdsCpu cpu, uint32_t return_pc) noexcept
{
    if (!initialized_) return NdsMachineStatus::NotInitialized;
    if (!valid_cpu(cpu)) return NdsMachineStatus::InvalidCpu;

    const unsigned index = cpu_index(cpu);
    if (slice_active_[index ^ 1u]) {
        return NdsMachineStatus::SliceAlreadyActive;
    }

    if (!cpu_can_execute(cpu)) return NdsMachineStatus::CpuHalted;
    ArmCpuState *state = cpu_state(cpu);
    if (state == nullptr) return NdsMachineStatus::InvalidCpu;
    if (!interrupt_control_.irq_asserted(cpu) ||
        (state->cpsr & CPSR_I_BIT) != 0u) {
        return NdsMachineStatus::IrqNotEligible;
    }

    const uint32_t alignment =
        (state->cpsr & CPSR_T_BIT) != 0u ? 2u : 4u;
    if ((return_pc % alignment) != 0u ||
        return_pc > UINT32_MAX - UINT32_C(4)) {
        return NdsMachineStatus::IrqEntryFailed;
    }

    if (!oracis_arm_runtime_select_cpu(cpu)) {
        return NdsMachineStatus::SchedulerFailure;
    }

    const uint32_t irq_link = return_pc + UINT32_C(4);
    return interrupt_control_.try_enter_irq(
        cpu, *state, irq_link, exception_vector_base(cpu))
        ? NdsMachineStatus::IrqEntered
        : NdsMachineStatus::IrqEntryFailed;
}

int NdsMachine::runtime_irq_handler(
    void *context, NdsCpu cpu, uint32_t return_pc)
{
    auto *machine = static_cast<NdsMachine *>(context);
    if (machine == nullptr || !machine->initialized_ ||
        machine->cpu_state(cpu) == nullptr ||
        oracis_active_cpu_state != machine->cpu_state(cpu)) {
        return -1;
    }

    const bool dispatch_now = machine->cpu_slice_active(cpu);
    const NdsMachineStatus status =
        machine->enter_runtime_irq(cpu, return_pc);
    if (status == NdsMachineStatus::IrqNotEligible ||
        status == NdsMachineStatus::CpuHalted) {
        return 0;
    }
    if (status != NdsMachineStatus::IrqEntered) return -1;

    if (dispatch_now) {
        ArmCpuState *state = machine->cpu_state(cpu);
        if (state == nullptr ||
            !oracis_arm_runtime_select_cpu(cpu)) {
            return -1;
        }
        runtime_dispatch(state->R[15]);
    }
    return 1;
}

uint32_t NdsMachine::exception_vector_base(NdsCpu cpu) const noexcept
{
    if (cpu == NDS_ARM9) {
        return (arm9_tcm_.control() &
                memory::NdsArm9Tcm::AlternateVectorSelectMask) != 0u
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