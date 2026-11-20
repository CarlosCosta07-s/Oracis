#include "oracis/memory/nds_display_control.hpp"
#include "oracis/memory/nds_dma_control.hpp"

#include <algorithm>

namespace oracis::memory {
namespace {

constexpr uint32_t kDispcntBytes = 4u;
constexpr uint32_t kBgRegisterBytes = NdsDisplayControl::BgCount * 2u;
constexpr uint32_t kBgScrollBytes = NdsDisplayControl::BgCount * 4u;
constexpr uint32_t kBgAffineBytes = UINT32_C(0x0020);
constexpr uint16_t kVblankFlag = UINT16_C(1u << 0u);
constexpr uint16_t kHblankFlag = UINT16_C(1u << 1u);
constexpr uint16_t kVcountMatchFlag = UINT16_C(1u << 2u);

constexpr uint32_t kIrqVblank = (UINT32_C(1) << 0u);
constexpr uint32_t kIrqHblank = (UINT32_C(1) << 1u);
constexpr uint32_t kIrqVcount = (UINT32_C(1) << 2u);

} // namespace

bool NdsDisplayControl::valid_cpu(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 || cpu == NDS_ARM7;
}

unsigned NdsDisplayControl::cpu_index(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 ? 0u : 1u;
}

void NdsDisplayControl::clear_main_memory_display_fifo() noexcept
{
    main_memory_display_fifo_ = {};
    main_memory_display_fifo_read_index_ = 0u;
    main_memory_display_fifo_write_index_ = 0u;
    main_memory_display_fifo_count_ = 0u;
    main_memory_display_pixel_half_ = 0u;
}

Status NdsDisplayControl::enqueue_main_memory_display_block(
    const uint32_t *words) noexcept
{
    if (!installed_) return Status::Unmapped;
    if (words == nullptr) return Status::InvalidRegion;
    if (main_memory_display_fifo_count_ != 0u) {
        return Status::PermissionDenied;
    }
    for (uint32_t index = 0u;
         index < MainMemoryDisplayFifoCapacityWords; ++index) {
        main_memory_display_fifo_[index] = words[index];
    }
    main_memory_display_fifo_read_index_ = 0u;
    main_memory_display_fifo_write_index_ = 0u;
    main_memory_display_fifo_count_ = MainMemoryDisplayFifoCapacityWords;
    main_memory_display_pixel_half_ = 0u;
    return Status::Ok;
}

ReadResult NdsDisplayControl::consume_main_memory_display_pixel() noexcept
{
    if (!installed_) return {Status::Unmapped, 0u};
    if (!main_memory_display_active_ || !main_memory_display_mode() ||
        (engines_[0].dispcnt & UINT32_C(0x80000000)) != 0u ||
        vcount_ >= VisibleScanlines) {
        return {Status::PermissionDenied, 0u};
    }
    if (main_memory_display_fifo_count_ == 0u) {
        if (dma_ == nullptr) return {Status::Unmapped, 0u};
        const Status refill = dma_->trigger_main_memory_display();
        if (refill != Status::Ok) return {refill, 0u};
        if (main_memory_display_fifo_count_ == 0u) {
            return {Status::Unmapped, 0u};
        }
    }

    const uint32_t word =
        main_memory_display_fifo_[main_memory_display_fifo_read_index_];
    const uint32_t shift = main_memory_display_pixel_half_ * 16u;
    const uint32_t pixel = (word >> shift) & UINT32_C(0x7FFF);
    ++main_memory_display_pixel_half_;
    if (main_memory_display_pixel_half_ == 2u) {
        main_memory_display_pixel_half_ = 0u;
        main_memory_display_fifo_read_index_ =
            (main_memory_display_fifo_read_index_ + 1u) %
            MainMemoryDisplayFifoCapacityWords;
        --main_memory_display_fifo_count_;
    }
    return {Status::Ok, pixel};
}

bool NdsDisplayControl::main_memory_display_mode() const noexcept
{
    return (engines_[0].dispcnt & UINT32_C(0x00030000)) ==
        UINT32_C(0x00030000);
}

Status NdsDisplayControl::copy_main_memory_display_scanline(
    uint32_t scanline, uint16_t *pixels,
    uint32_t pixel_capacity) const noexcept
{
    if (!installed_) return Status::Unmapped;
    if (pixels == nullptr || pixel_capacity < MainMemoryDisplayWidth ||
        scanline >= MainMemoryDisplayHeight) {
        return Status::InvalidRegion;
    }
    if (!main_memory_display_mode()) return Status::PermissionDenied;
    const uint32_t row_offset = scanline * MainMemoryDisplayWidth;
    if ((engines_[0].dispcnt & UINT32_C(0x80000000)) != 0u) {
        for (uint32_t x = 0u; x < MainMemoryDisplayWidth; ++x) {
            pixels[x] = 0u;
        }
        return Status::Ok;
    }
    for (uint32_t x = 0u; x < MainMemoryDisplayWidth; ++x) {
        pixels[x] = main_memory_display_frame_[row_offset + x];
    }
    return Status::Ok;
}

void NdsDisplayControl::advance_main_memory_display_pixel() noexcept
{
    if (!main_memory_display_active_ || !main_memory_display_mode() ||
        (engines_[0].dispcnt & UINT32_C(0x80000000)) != 0u ||
        vcount_ >= VisibleScanlines ||
        main_memory_display_pixel_index_ >= MainMemoryDisplayWidth) {
        return;
    }

    const ReadResult pixel = consume_main_memory_display_pixel();
    const uint32_t offset =
        static_cast<uint32_t>(vcount_) * MainMemoryDisplayWidth +
        main_memory_display_pixel_index_;
    main_memory_display_frame_[offset] = pixel ?
        static_cast<uint16_t>(pixel.value & UINT32_C(0x7FFF)) : 0u;
    ++main_memory_display_pixel_index_;
}

uint32_t NdsDisplayControl::dispcnt(unsigned engine_index) const noexcept
{
    if (engine_index >= engines_.size()) return 0u;
    return engines_[engine_index].dispcnt;
}

uint16_t NdsDisplayControl::bg_control(
    unsigned engine_index, unsigned bg_index) const noexcept
{
    if (engine_index >= engines_.size() || bg_index >= BgCount) return 0u;
    return engines_[engine_index].bgcnt[bg_index];
}

NdsDisplayControl::BgScrollState NdsDisplayControl::bg_scroll(
    unsigned engine_index, unsigned bg_index) const noexcept
{
    if (engine_index >= engines_.size() || bg_index >= BgCount) return {};
    const EngineState &engine = engines_[engine_index];
    return {engine.bg_hofs[bg_index], engine.bg_vofs[bg_index]};
}

NdsDisplayControl::BgAffineState NdsDisplayControl::bg_affine(
    unsigned engine_index, unsigned bg_index) const noexcept
{
    if (engine_index >= engines_.size() || bg_index < 2u || bg_index > 3u) {
        return {};
    }
    return engines_[engine_index].bg_affine[bg_index - 2u];
}

NdsDisplayControl::WindowBlendState NdsDisplayControl::window_blend(
    unsigned engine_index) const noexcept
{
    if (engine_index >= engines_.size()) return {};
    return engines_[engine_index].window_blend;
}

namespace {

int32_t sign_extend_affine_reference(uint32_t value) noexcept
{
    const uint32_t raw = value & NdsDisplayControl::BgAffineReferenceMask;
    if ((raw & UINT32_C(0x08000000)) == 0u) {
        return static_cast<int32_t>(raw);
    }
    const int64_t signed_value =
        static_cast<int64_t>(raw) - INT64_C(0x10000000);
    return static_cast<int32_t>(signed_value);
}

} // namespace

bool NdsDisplayControl::locate_engine_register(
    uint32_t address,
    unsigned &engine_index,
    uint32_t &register_offset) noexcept
{
    uint32_t base = 0u;
    if (address >= EngineABase &&
        address < EngineABase + UINT32_C(0x1000)) {
        engine_index = 0u;
        base = EngineABase;
    } else if (address >= EngineBBase &&
               address < EngineBBase + UINT32_C(0x1000)) {
        engine_index = 1u;
        base = EngineBBase;
    } else {
        return false;
    }

    register_offset = address - base;
    if (register_offset < kDispcntBytes) return true;
    if (register_offset >= BgControlOffset &&
        register_offset < BgControlOffset + kBgRegisterBytes) {
        return true;
    }
    if (register_offset >= BgScrollOffset &&
        register_offset < BgScrollOffset + kBgScrollBytes) {
        return true;
    }
    if (register_offset >= BgAffineOffset &&
        register_offset < BgAffineOffset + kBgAffineBytes) {
        return true;
    }
    return (register_offset >= WindowBoundsOffset &&
            register_offset <= WindowBoundsOffset + UINT32_C(0x0006) &&
            (register_offset & 1u) == 0u) ||
           register_offset == WininOffset ||
           register_offset == WinoutOffset ||
           register_offset == MosaicOffset ||
           register_offset == BldcntOffset ||
           register_offset == BldalphaOffset ||
           register_offset == BldyOffset;
}

bool NdsDisplayControl::is_vblank_line(uint16_t scanline) noexcept
{
    return scanline >= VisibleScanlines && scanline < LastScanline;
}

uint16_t NdsDisplayControl::vcount_compare(unsigned index) const noexcept
{
    const uint16_t control = cpu_status_[index].control;
    return static_cast<uint16_t>(
        ((control >> 8u) & UINT16_C(0x00FF)) |
        ((control & UINT16_C(0x0080)) << 1u));
}

bool NdsDisplayControl::vcount_matches(unsigned index) const noexcept
{
    return vcount_compare(index) == vcount_;
}

void NdsDisplayControl::request_irq_if_enabled(
    unsigned index, uint16_t enable_bit, uint32_t irq_mask) noexcept
{
    if ((cpu_status_[index].control & enable_bit) != 0u) {
        const NdsCpu cpu = index == 0u ? NDS_ARM9 : NDS_ARM7;
        static_cast<void>(interrupts_.request_irq(cpu, irq_mask));
    }
}

uint16_t NdsDisplayControl::read_dispstat(NdsCpu cpu) const noexcept
{
    const unsigned index = cpu_index(cpu);
    uint16_t value = cpu_status_[index].control;

    if (is_vblank_line(vcount_)) value |= kVblankFlag;

    const uint32_t hblank_start =
        cpu == NDS_ARM9 ? Arm9HblankStartCycle : Arm7HblankStartCycle;
    if (line_cycle_ >= hblank_start) value |= kHblankFlag;
    if (vcount_matches(index)) value |= kVcountMatchFlag;

    return value;
}

Status NdsDisplayControl::install() noexcept
{
    if (installed_) return Status::InvalidRegion;

    engines_ = {};
    cpu_status_ = {};
    reset_timing();
    installed_ = true;
    return Status::Ok;
}

ReadResult NdsDisplayControl::read(
    NdsCpu cpu, uint32_t address, uint32_t width) const noexcept
{
    if (address == MainMemoryDisplayFifoAddress) {
        if (!installed_) return {Status::Unmapped, 0u};
        if (!valid_cpu(cpu)) return {Status::InvalidCpu, 0u};
        if (cpu != NDS_ARM9) return {Status::Unmapped, 0u};
        if (width != 4u) return {Status::InvalidWidth, 0u};
        return {Status::PermissionDenied, 0u};
    }

    if (address == DispstatAddress || address == VcountAddress) {
        if (!installed_) return {Status::Unmapped, 0u};
        if (!valid_cpu(cpu)) return {Status::InvalidCpu, 0u};
        if (width != 2u) return {Status::InvalidWidth, 0u};
        return {
            Status::Ok,
            address == DispstatAddress
                ? read_dispstat(cpu)
                : static_cast<uint32_t>(vcount_ & VcountMask)
        };
    }

    unsigned engine_index = 0u;
    uint32_t offset = 0u;
    if (!locate_engine_register(address, engine_index, offset)) {
        return {Status::Unmapped, 0u};
    }
    if (!installed_) return {Status::Unmapped, 0u};
    if (!valid_cpu(cpu)) return {Status::InvalidCpu, 0u};

    /* DISPCNT and BGxCNT belong to the ARM9-accessible 2D engines. */
    if (cpu != NDS_ARM9) return {Status::Unmapped, 0u};

    const EngineState &engine = engines_[engine_index];
    if (offset == DispcntOffset) {
        if (width != 4u) return {Status::InvalidWidth, 0u};
        return {Status::Ok, engine.dispcnt};
    }

    if (offset >= BgControlOffset &&
        offset < BgControlOffset + kBgRegisterBytes) {
        const uint32_t bg_offset = offset - BgControlOffset;
        if ((bg_offset & 1u) == 0u && width == 2u) {
            const unsigned bg_index = static_cast<unsigned>(bg_offset / 2u);
            return {Status::Ok, engine.bgcnt[bg_index]};
        }
        if ((bg_offset & 3u) == 0u && width == 4u &&
            bg_offset + 4u <= kBgRegisterBytes) {
            const unsigned first_bg = static_cast<unsigned>(bg_offset / 2u);
            return {
                Status::Ok,
                static_cast<uint32_t>(engine.bgcnt[first_bg]) |
                    (static_cast<uint32_t>(engine.bgcnt[first_bg + 1u]) << 16u)
            };
        }
        return {Status::InvalidWidth, 0u};
    }

    if (offset >= BgScrollOffset &&
        offset < BgScrollOffset + kBgScrollBytes) {
        const uint32_t scroll_offset = offset - BgScrollOffset;
        if ((scroll_offset & 1u) == 0u && width == 2u) {
            return {Status::PermissionDenied, 0u}; /* BGxHOFS/BGxVOFS are write-only. */
        }
        return {Status::InvalidWidth, 0u};
    }

    if (offset >= BgAffineOffset &&
        offset < BgAffineOffset + kBgAffineBytes) {
        const uint32_t affine_offset = offset - BgAffineOffset;
        const uint32_t bg_offset = affine_offset % UINT32_C(0x10);
        if ((bg_offset < 8u && (bg_offset & 1u) == 0u && width == 2u) ||
            ((bg_offset == 8u || bg_offset == 12u) &&
             width == 4u)) {
            return {Status::PermissionDenied, 0u}; /* Affine registers are write-only. */
        }
        return {Status::InvalidWidth, 0u};
    }

    if (offset >= WindowBoundsOffset &&
        offset <= WindowBoundsOffset + UINT32_C(0x0006)) {
        if (width != 2u) return {Status::InvalidWidth, 0u};
        return {Status::PermissionDenied, 0u}; /* WIN0H/WIN1H/WIN0V/WIN1V are write-only. */
    }

    const WindowBlendState &effects = engine.window_blend;
    if (width != 2u) return {Status::InvalidWidth, 0u};
    switch (offset) {
    case WininOffset:
        return {Status::Ok, effects.winin};
    case WinoutOffset:
        return {Status::Ok, effects.winout};
    case MosaicOffset:
    case BldyOffset:
        return {Status::PermissionDenied, 0u};
    case BldcntOffset:
        return {Status::Ok, effects.bldcnt};
    case BldalphaOffset:
        return {Status::Ok, effects.bldalpha};
    default:
        return {Status::InvalidWidth, 0u};
    }
    return {Status::InvalidWidth, 0u};
}

Status NdsDisplayControl::write(
    NdsCpu cpu,
    uint32_t address,
    uint32_t width,
    uint32_t value) noexcept
{
    if (address == MainMemoryDisplayFifoAddress) {
        if (!installed_) return Status::Unmapped;
        if (!valid_cpu(cpu)) return Status::InvalidCpu;
        if (cpu != NDS_ARM9) return Status::Unmapped;
        if (width != 4u) return Status::InvalidWidth;
        if (main_memory_display_fifo_count_ >=
            MainMemoryDisplayFifoCapacityWords) {
            return Status::PermissionDenied;
        }
        main_memory_display_fifo_[main_memory_display_fifo_write_index_] = value;
        main_memory_display_fifo_write_index_ =
            (main_memory_display_fifo_write_index_ + 1u) %
            MainMemoryDisplayFifoCapacityWords;
        ++main_memory_display_fifo_count_;
        return Status::Ok;
    }

    if (address == DispstatAddress) {
        if (!installed_) return Status::Unmapped;
        if (!valid_cpu(cpu)) return Status::InvalidCpu;
        if (width != 2u) return Status::InvalidWidth;

        const unsigned index = cpu_index(cpu);
        const bool matched_before = vcount_matches(index);
        cpu_status_[index].control =
            static_cast<uint16_t>(value) & DispstatWritableMask;
        const bool matched_after = vcount_matches(index);
        if (!matched_before && matched_after) {
            request_irq_if_enabled(
                index, VcountIrqEnable, kIrqVcount);
        }
        return Status::Ok;
    }

    if (address == VcountAddress) {
        if (!installed_) return Status::Unmapped;
        if (!valid_cpu(cpu)) return Status::InvalidCpu;
        if (width != 2u) return Status::InvalidWidth;

        /*
         * VCOUNT is writable. The documented 202..212 interval is the
         * recommended synchronization window, not an access restriction.
         * Queue changes to the next scanline boundary to keep the local
         * scanline clock coherent with the shared bus-time watermark.
         */
        const uint16_t requested =
            static_cast<uint16_t>(value) & VcountMask;
        if (requested > LastScanline) return Status::PermissionDenied;

        pending_vcount_ = requested;
        pending_vcount_write_ = true;
        return Status::Ok;
    }

    unsigned engine_index = 0u;
    uint32_t offset = 0u;
    if (!locate_engine_register(address, engine_index, offset)) {
        return Status::Unmapped;
    }
    if (!installed_) return Status::Unmapped;
    if (!valid_cpu(cpu)) return Status::InvalidCpu;
    if (cpu != NDS_ARM9) return Status::Unmapped;

    EngineState &engine = engines_[engine_index];
    if (offset == DispcntOffset) {
        if (width != 4u) return Status::InvalidWidth;
        const uint32_t next_value = engine_index == 0u
            ? value : (value & EngineBDispcntSupportedMask);
        if (engine_index == 0u) {
            const bool was_mmd_output =
                (engine.dispcnt & UINT32_C(0x00030000)) ==
                    UINT32_C(0x00030000) &&
                (engine.dispcnt & UINT32_C(0x80000000)) == 0u;
            const bool becomes_mmd_output =
                (next_value & UINT32_C(0x00030000)) ==
                    UINT32_C(0x00030000) &&
                (next_value & UINT32_C(0x80000000)) == 0u;
            if (was_mmd_output != becomes_mmd_output ||
                ((engine.dispcnt ^ next_value) & UINT32_C(0x00030000)) != 0u) {
                main_memory_display_active_ = false;
                main_memory_display_pixel_index_ = 0u;
                clear_main_memory_display_fifo();
                main_memory_display_frame_.fill(0u);
            }
        }
        engine.dispcnt = next_value;
        return Status::Ok;
    }

    if (offset >= BgControlOffset &&
        offset < BgControlOffset + kBgRegisterBytes) {
        const uint32_t bg_offset = offset - BgControlOffset;
        if ((bg_offset & 1u) == 0u && width == 2u) {
            const unsigned bg_index = static_cast<unsigned>(bg_offset / 2u);
            engine.bgcnt[bg_index] = static_cast<uint16_t>(value);
            return Status::Ok;
        }
        if ((bg_offset & 3u) == 0u && width == 4u &&
            bg_offset + 4u <= kBgRegisterBytes) {
            const unsigned first_bg = static_cast<unsigned>(bg_offset / 2u);
            engine.bgcnt[first_bg] = static_cast<uint16_t>(value);
            engine.bgcnt[first_bg + 1u] =
                static_cast<uint16_t>(value >> 16u);
            return Status::Ok;
        }
        return Status::InvalidWidth;
    }

    if (offset >= BgScrollOffset &&
        offset < BgScrollOffset + kBgScrollBytes) {
        const uint32_t scroll_offset = offset - BgScrollOffset;
        if ((scroll_offset & 1u) == 0u && width == 2u) {
            const unsigned bg_index = static_cast<unsigned>(scroll_offset / 4u);
            const unsigned component = static_cast<unsigned>(scroll_offset % 4u);
            const uint16_t scroll_value =
                static_cast<uint16_t>(value) & BgScrollValueMask;
            if (component == 0u) {
                engine.bg_hofs[bg_index] = scroll_value;
                return Status::Ok;
            }
            if (component == 2u) {
                engine.bg_vofs[bg_index] = scroll_value;
                return Status::Ok;
            }
        }
        return Status::InvalidWidth;
    }

    if (offset >= BgAffineOffset &&
        offset < BgAffineOffset + kBgAffineBytes) {
        const uint32_t affine_offset = offset - BgAffineOffset;
        const unsigned bg_index =
            affine_offset >= UINT32_C(0x10) ? 1u : 0u;
        const uint32_t bg_offset = affine_offset % UINT32_C(0x10);
        BgAffineState &affine = engine.bg_affine[bg_index];

        if (bg_offset < 8u && (bg_offset & 1u) == 0u && width == 2u) {
            const int16_t coefficient = static_cast<int16_t>(
                static_cast<uint16_t>(value));
            switch (bg_offset / 2u) {
            case 0u: affine.pa = coefficient; return Status::Ok;
            case 1u: affine.pb = coefficient; return Status::Ok;
            case 2u: affine.pc = coefficient; return Status::Ok;
            case 3u: affine.pd = coefficient; return Status::Ok;
            default: break;
            }
        }

        if ((bg_offset == 8u || bg_offset == 12u) && width == 4u) {
            const int32_t coordinate = sign_extend_affine_reference(value);
            if (bg_offset == 8u) {
                affine.reference_x = coordinate;
                if (vcount_ < VisibleScanlines) {
                    affine.line_reference_x = coordinate;
                }
            } else {
                affine.reference_y = coordinate;
                if (vcount_ < VisibleScanlines) {
                    affine.line_reference_y = coordinate;
                }
            }
            return Status::Ok;
        }
        return Status::InvalidWidth;
    }

    if (offset >= WindowBoundsOffset &&
        offset <= WindowBoundsOffset + UINT32_C(0x0006)) {
        if (width != 2u) return Status::InvalidWidth;
        const unsigned bound_index =
            static_cast<unsigned>((offset - WindowBoundsOffset) / 2u);
        engine.window_blend.bounds[bound_index] = static_cast<uint16_t>(value);
        return Status::Ok;
    }

    if (width != 2u) return Status::InvalidWidth;
    WindowBlendState &effects = engine.window_blend;
    switch (offset) {
    case WininOffset:
        effects.winin = static_cast<uint16_t>(value) & WindowMask;
        return Status::Ok;
    case WinoutOffset:
        effects.winout = static_cast<uint16_t>(value) & WindowMask;
        return Status::Ok;
    case MosaicOffset:
        effects.mosaic = static_cast<uint16_t>(value);
        return Status::Ok;
    case BldcntOffset:
        effects.bldcnt = static_cast<uint16_t>(value) & BlendControlMask;
        return Status::Ok;
    case BldalphaOffset:
        effects.bldalpha = static_cast<uint16_t>(value) & BlendAlphaMask;
        return Status::Ok;
    case BldyOffset:
        effects.bldy = static_cast<uint16_t>(value) & BlendYMask;
        return Status::Ok;
    default:
        return Status::InvalidWidth;
    }
    return Status::InvalidWidth;
}

void NdsDisplayControl::advance_scanline() noexcept
{
    const std::array<bool, 2> vcount_match_before = {
        vcount_matches(0u), vcount_matches(1u)
    };

    const bool natural_frame_wrap =
        vcount_ == LastScanline && !pending_vcount_write_;
    uint16_t next_scanline = vcount_ == LastScanline
        ? 0u : static_cast<uint16_t>(vcount_ + 1u);
    if (pending_vcount_write_) {
        next_scanline = pending_vcount_;
        pending_vcount_write_ = false;
    }
    /*
     * BG2/BG3 reference points have an internal scanline accumulator.
     * PB/PD advance it at each line boundary. The programmed 19.8 reference
     * is reloaded at frame wrap; visible-line BGxX/BGxY writes replace the
     * current internal origin immediately in the register-write path.
     */
    for (EngineState &engine : engines_) {
        for (BgAffineState &affine : engine.bg_affine) {
            if (next_scanline == 0u) {
                affine.line_reference_x = affine.reference_x;
                affine.line_reference_y = affine.reference_y;
            } else {
                const int64_t next_x =
                    static_cast<int64_t>(affine.line_reference_x) + affine.pb;
                const int64_t next_y =
                    static_cast<int64_t>(affine.line_reference_y) + affine.pd;
                affine.line_reference_x = sign_extend_affine_reference(
                    static_cast<uint32_t>(next_x));
                affine.line_reference_y = sign_extend_affine_reference(
                    static_cast<uint32_t>(next_y));
            }
        }
    }

    vcount_ = next_scanline;
    main_memory_display_pixel_index_ = 0u;
    if (natural_frame_wrap && vcount_ == 0u) {
        main_memory_display_active_ = main_memory_display_mode() &&
            (engines_[0].dispcnt & UINT32_C(0x80000000)) == 0u;
        main_memory_display_frame_.fill(0u);
        clear_main_memory_display_fifo();
    }
    if (vcount_ < VisibleScanlines) {
        advance_main_memory_display_pixel();
    }

    /*
     * NDS9 DMA timing 3 is synchronized to the start of each visible line.
     * It is not a timer-overflow trigger (that belongs to DSi NDMA).
     */
    if (dma_ != nullptr && vcount_ < VisibleScanlines) {
        static_cast<void>(dma_->trigger_display_sync(NDS_ARM9));
    }

    /* The VBlank IRQ is tied to the real line-192 edge, not merely a
     * software VCOUNT jump into the VBlank range. The status flag remains
     * derived from the current line through read_dispstat(). */
    if (vcount_ == VisibleScanlines) {
        if (dma_ != nullptr) {
            static_cast<void>(dma_->trigger_vblank(NDS_ARM9));
            static_cast<void>(dma_->trigger_vblank(NDS_ARM7));
        }
        clear_main_memory_display_fifo();
        main_memory_display_active_ = false;
        main_memory_display_pixel_index_ = 0u;
        request_irq_if_enabled(0u, VblankIrqEnable, kIrqVblank);
        request_irq_if_enabled(1u, VblankIrqEnable, kIrqVblank);
    }

    for (unsigned index = 0u; index < cpu_status_.size(); ++index) {
        if (!vcount_match_before[index] && vcount_matches(index)) {
            request_irq_if_enabled(
                index, VcountIrqEnable, kIrqVcount);
        }
    }

    hblank_irq_fired_ = {};
}

void NdsDisplayControl::advance_bus_cycles(uint64_t cycles) noexcept
{
    if (!installed_ || cycles == 0u) return;

    while (cycles != 0u) {
        uint32_t next_event = CyclesPerScanline - line_cycle_;

        if (main_memory_display_active_ && main_memory_display_mode() &&
            vcount_ < VisibleScanlines &&
            main_memory_display_pixel_index_ < MainMemoryDisplayWidth) {
            const uint32_t next_pixel_cycle = static_cast<uint32_t>(
                (static_cast<uint64_t>(main_memory_display_pixel_index_) *
                    Arm9HblankStartCycle + 255u) / MainMemoryDisplayWidth);
            if (next_pixel_cycle > line_cycle_) {
                next_event = std::min(
                    next_event, next_pixel_cycle - line_cycle_);
            } else {
                advance_main_memory_display_pixel();
                continue;
            }
        }

        if (!hblank_irq_fired_[0] &&
            line_cycle_ < Arm9HblankStartCycle) {
            next_event = std::min(
                next_event, Arm9HblankStartCycle - line_cycle_);
        }
        if (!hblank_irq_fired_[1] &&
            line_cycle_ < Arm7HblankStartCycle) {
            next_event = std::min(
                next_event, Arm7HblankStartCycle - line_cycle_);
        }

        const uint32_t step = static_cast<uint32_t>(
            std::min<uint64_t>(cycles, next_event));
        line_cycle_ += step;
        cycles -= step;

        if (main_memory_display_active_ && main_memory_display_mode() &&
            vcount_ < VisibleScanlines &&
            main_memory_display_pixel_index_ < MainMemoryDisplayWidth) {
            const uint32_t next_pixel_cycle = static_cast<uint32_t>(
                (static_cast<uint64_t>(main_memory_display_pixel_index_) *
                    Arm9HblankStartCycle + 255u) / MainMemoryDisplayWidth);
            if (line_cycle_ == next_pixel_cycle) {
                advance_main_memory_display_pixel();
            }
        }

        if (line_cycle_ == Arm9HblankStartCycle &&
            !hblank_irq_fired_[0]) {
            hblank_irq_fired_[0] = true;
            if (dma_ != nullptr && vcount_ < VisibleScanlines) {
                static_cast<void>(dma_->trigger_hblank(NDS_ARM9));
            }
            request_irq_if_enabled(0u, HblankIrqEnable, kIrqHblank);
        }
        if (line_cycle_ == Arm7HblankStartCycle &&
            !hblank_irq_fired_[1]) {
            hblank_irq_fired_[1] = true;
            request_irq_if_enabled(1u, HblankIrqEnable, kIrqHblank);
        }

        if (line_cycle_ == CyclesPerScanline) {
            line_cycle_ = 0u;
            advance_scanline();
        }
    }
}

void NdsDisplayControl::reset_timing() noexcept
{
    clear_main_memory_display_fifo();
    main_memory_display_frame_.fill(0u);
    main_memory_display_pixel_index_ = 0u;
    main_memory_display_active_ = false;
    vcount_ = 0u;
    line_cycle_ = 0u;
    pending_vcount_write_ = false;
    pending_vcount_ = 0u;
    hblank_irq_fired_ = {};
    for (EngineState &engine : engines_) {
        for (BgAffineState &affine : engine.bg_affine) {
            affine.line_reference_x = affine.reference_x;
            affine.line_reference_y = affine.reference_y;
        }
    }
}

} // namespace oracis::memory