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

class NdsDisplayControl final {
public:
    static constexpr uint32_t EngineABase = UINT32_C(0x04000000);
    static constexpr uint32_t EngineBBase = UINT32_C(0x04001000);
    static constexpr uint32_t DispcntOffset = UINT32_C(0x0000);
    static constexpr uint32_t DispstatAddress = UINT32_C(0x04000004);
    static constexpr uint32_t VcountAddress = UINT32_C(0x04000006);
    static constexpr uint32_t BgControlOffset = UINT32_C(0x0008);
    static constexpr uint32_t BgCount = 4u;

    static constexpr uint32_t EngineBDispcntSupportedMask =
        UINT32_C(0xC0B3FFF7);

    static constexpr uint32_t CyclesPerScanline = 2130u;
    static constexpr uint32_t Arm9HblankStartCycle = 1606u;
    static constexpr uint32_t Arm7HblankStartCycle = 1613u;
    static constexpr uint16_t VisibleScanlines = 192u;
    static constexpr uint16_t ScanlinesPerFrame = 263u;
    static constexpr uint16_t LastScanline = ScanlinesPerFrame - 1u;

    explicit NdsDisplayControl(NdsInterruptControl &interrupts) noexcept
        : interrupts_(interrupts) {}
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

    NdsInterruptControl &interrupt_control() const noexcept {
        return interrupts_;
    }
    bool installed() const noexcept { return installed_; }
    uint16_t current_scanline() const noexcept { return vcount_; }
    uint32_t scanline_cycle() const noexcept { return line_cycle_; }

private:
    struct EngineState {
        uint32_t dispcnt = 0u;
        std::array<uint16_t, BgCount> bgcnt{};
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
    void request_irq_if_enabled(
        unsigned cpu_index, uint16_t enable_bit, uint32_t irq_mask) noexcept;

    NdsInterruptControl &interrupts_;
    std::array<EngineState, 2> engines_{};
    std::array<CpuStatus, 2> cpu_status_{};
    std::array<bool, 2> hblank_irq_fired_{};
    uint16_t vcount_ = 0u;
    uint32_t line_cycle_ = 0u;
    bool pending_vcount_write_ = false;
    uint16_t pending_vcount_ = 0u;
    bool installed_ = false;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_DISPLAY_CONTROL_HPP */
