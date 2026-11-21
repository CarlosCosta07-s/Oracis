/*
 * Nintendo DS 2D display-engine registers and scanline status.
 *
 * Configuration registers belong to Engines A/B. DISPSTAT/VCOUNT are the
 * shared Engine-A-port status registers visible to both NDS processors.
 * Rendering and PPU fetches remain separate components.
 */
#ifndef ORACIS_MEMORY_NDS_DISPLAY_CONTROL_HPP
#define ORACIS_MEMORY_NDS_DISPLAY_CONTROL_HPP

#include <array>
#include <cstdint>

#include "oracis/memory/guest_memory.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"

namespace oracis::memory {

class NdsDmaControl;

class NdsDisplayControl final {
public:
    static constexpr uint32_t EngineABase = UINT32_C(0x04000000);
    static constexpr uint32_t EngineBBase = UINT32_C(0x04001000);
    static constexpr uint32_t DispcntOffset = UINT32_C(0x0000);
    static constexpr uint32_t DispstatAddress = UINT32_C(0x04000004);
    static constexpr uint32_t VcountAddress = UINT32_C(0x04000006);
    static constexpr uint32_t BgControlOffset = UINT32_C(0x0008);
    static constexpr uint32_t BgScrollOffset = UINT32_C(0x0010);
    static constexpr uint32_t BgAffineOffset = UINT32_C(0x0020);
    static constexpr uint32_t WindowBoundsOffset = UINT32_C(0x0040);
    static constexpr uint32_t WininOffset = UINT32_C(0x0048);
    static constexpr uint32_t WinoutOffset = UINT32_C(0x004A);
    static constexpr uint32_t MosaicOffset = UINT32_C(0x004C);
    static constexpr uint32_t BldcntOffset = UINT32_C(0x0050);
    static constexpr uint32_t BldalphaOffset = UINT32_C(0x0052);
    static constexpr uint32_t BldyOffset = UINT32_C(0x0054);
    static constexpr uint32_t MasterBrightnessOffset = UINT32_C(0x006C);
    static constexpr uint32_t MainMemoryDisplayFifoAddress =
        UINT32_C(0x04000068);
    static constexpr uint32_t MainMemoryDisplayFifoCapacityWords = 4u;
    static constexpr uint32_t MainMemoryDisplayWidth = 256u;
    static constexpr uint32_t MainMemoryDisplayHeight = 192u;
    static constexpr uint32_t MainMemoryDisplayCyclesPerPixel = 6u;
    static constexpr uint32_t MainMemoryDisplayActiveCycles =
        MainMemoryDisplayWidth * MainMemoryDisplayCyclesPerPixel;
    static constexpr uint32_t BgCount = 4u;
    static constexpr uint16_t BgScrollValueMask = UINT16_C(0x01FF);
    static constexpr uint32_t BgAffineReferenceMask = UINT32_C(0x0FFFFFFF);
    static constexpr uint16_t WindowMask = UINT16_C(0x3F3F);
    static constexpr uint16_t BlendControlMask = UINT16_C(0x3FFF);
    static constexpr uint16_t BlendAlphaMask = UINT16_C(0x1F1F);
    static constexpr uint16_t BlendYMask = UINT16_C(0x001F);
    static constexpr uint16_t MasterBrightnessMask = UINT16_C(0xC01F);

    struct BgScrollState {
        uint16_t horizontal = 0u;
        uint16_t vertical = 0u;
    };

    struct BgAffineState {
        int16_t pa = 0;
        int16_t pb = 0;
        int16_t pc = 0;
        int16_t pd = 0;
        int32_t reference_x = 0;
        int32_t reference_y = 0;
        /* Internal affine reference point for the currently rendered line. */
        int32_t line_reference_x = 0;
        int32_t line_reference_y = 0;
    };

    struct WindowBlendState {
        /* WIN0H, WIN1H, WIN0V, WIN1V in register order. */
        std::array<uint16_t, 4> bounds{};
        uint16_t winin = 0u;
        uint16_t winout = 0u;
        uint16_t mosaic = 0u;
        uint16_t bldcnt = 0u;
        uint16_t bldalpha = 0u;
        uint16_t bldy = 0u;
    };

    static constexpr uint32_t EngineBDispcntSupportedMask =
        UINT32_C(0xC0B3FFF7);

    static constexpr uint32_t CyclesPerScanline = 2130u;
    static constexpr uint32_t Arm9HblankStartCycle = 1606u;
    static constexpr uint32_t Arm7HblankStartCycle = 1613u;
    static constexpr uint16_t VisibleScanlines = 192u;
    static constexpr uint16_t ScanlinesPerFrame = 263u;
    static constexpr uint16_t LastScanline = ScanlinesPerFrame - 1u;

    explicit NdsDisplayControl(
        NdsInterruptControl &interrupts,
        NdsDmaControl *dma = nullptr) noexcept
        : interrupts_(interrupts), dma_(dma) {}
    NdsDisplayControl(const NdsDisplayControl &) = delete;
    NdsDisplayControl &operator=(const NdsDisplayControl &) = delete;

    Status install() noexcept;

    ReadResult read(NdsCpu cpu,
                    uint32_t address,
                    uint32_t width) const noexcept;

    Status write(NdsCpu cpu,
                 uint32_t address,
                 uint32_t width,
                 uint32_t value) noexcept;

    /*
     * Called only by NdsSystemClock at its synchronized bus-time watermark.
     * VCOUNT synchronization writes are queued to the next line boundary;
     * lines 202..212 are the recommended, not exclusive, write window.
     */
    void advance_bus_cycles(uint64_t cycles) noexcept;
    void reset_timing() noexcept;

    /* True when Engine A selects the main-memory display input mode. */
    bool main_memory_display_mode() const noexcept;
    /* Copy a clock-generated Engine A MMD framebuffer scanline for the host. */
    Status copy_main_memory_display_scanline(
        uint32_t scanline, uint16_t *pixels,
        uint32_t pixel_capacity) const noexcept;
    /* Consume one RGB555 pixel from Engine A's main-memory display FIFO. */
    ReadResult consume_main_memory_display_pixel() noexcept;
    uint32_t main_memory_display_fifo_words() const noexcept {
        return main_memory_display_fifo_count_;
    }
    Status enqueue_main_memory_display_block(
        const uint32_t *words) noexcept;
    void clear_main_memory_display_fifo() noexcept;

    NdsInterruptControl &interrupt_control() const noexcept {
        return interrupts_;
    }
    bool installed() const noexcept { return installed_; }
    uint32_t dispcnt(unsigned engine_index) const noexcept;
    uint16_t master_brightness(unsigned engine_index) const noexcept;
    uint16_t apply_master_brightness(
        unsigned engine_index, uint16_t rgb555) const noexcept;
    uint16_t bg_control(
        unsigned engine_index, unsigned bg_index) const noexcept;
    uint16_t current_scanline() const noexcept { return vcount_; }
    uint32_t scanline_cycle() const noexcept { return line_cycle_; }
    BgScrollState bg_scroll(
        unsigned engine_index, unsigned bg_index) const noexcept;
    BgAffineState bg_affine(
        unsigned engine_index, unsigned bg_index) const noexcept;
    WindowBlendState window_blend(
        unsigned engine_index) const noexcept;

private:
    struct EngineState {
        uint32_t dispcnt = 0u;
        uint16_t master_brightness = 0u;
        std::array<uint16_t, BgCount> bgcnt{};
        std::array<uint16_t, BgCount> bg_hofs{};
        std::array<uint16_t, BgCount> bg_vofs{};
        std::array<BgAffineState, 2> bg_affine{};
        WindowBlendState window_blend{};
    };

    struct CpuStatus {
        /* Only writable DISPSTAT bits; status bits are synthesized on read. */
        uint16_t control = 0u;
    };

    static constexpr uint16_t DispstatWritableMask = UINT16_C(0xFFB8);
    static constexpr uint16_t VcountMask = UINT16_C(0x01FF);
    static constexpr uint16_t HblankIrqEnable = UINT16_C(1u << 4u);
    static constexpr uint16_t VblankIrqEnable = UINT16_C(1u << 3u);
    static constexpr uint16_t VcountIrqEnable = UINT16_C(1u << 5u);

    static bool valid_cpu(NdsCpu cpu) noexcept;
    static unsigned cpu_index(NdsCpu cpu) noexcept;
    static bool locate_engine_register(
        uint32_t address, unsigned &engine_index,
        uint32_t &register_offset) noexcept;
    static bool is_vblank_line(uint16_t scanline) noexcept;

    uint16_t vcount_compare(unsigned cpu_index) const noexcept;
    bool vcount_matches(unsigned cpu_index) const noexcept;
    uint16_t read_dispstat(NdsCpu cpu) const noexcept;
    void advance_scanline() noexcept;
    void advance_main_memory_display_pixel() noexcept;
    void request_irq_if_enabled(
        unsigned cpu_index, uint16_t enable_bit, uint32_t irq_mask) noexcept;

    NdsInterruptControl &interrupts_;
    NdsDmaControl *dma_ = nullptr;
    std::array<EngineState, 2> engines_{};
    std::array<CpuStatus, 2> cpu_status_{};
    std::array<bool, 2> hblank_irq_fired_{};
    std::array<uint32_t, MainMemoryDisplayFifoCapacityWords>
        main_memory_display_fifo_{};
    uint32_t main_memory_display_fifo_read_index_ = 0u;
    uint32_t main_memory_display_fifo_write_index_ = 0u;
    uint32_t main_memory_display_fifo_count_ = 0u;
    uint32_t main_memory_display_pixel_half_ = 0u;
    std::array<uint16_t, MainMemoryDisplayWidth * MainMemoryDisplayHeight>
        main_memory_display_frame_{};
    uint32_t main_memory_display_pixel_index_ = 0u;
    bool main_memory_display_active_ = false;
    uint16_t vcount_ = 0u;
    uint32_t line_cycle_ = 0u;
    bool pending_vcount_write_ = false;
    uint16_t pending_vcount_ = 0u;
    bool installed_ = false;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_DISPLAY_CONTROL_HPP */