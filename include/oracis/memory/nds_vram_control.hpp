/*
 * Nintendo DS VRAM bank controller.
 *
 * This partial device maps all nine banks to ARM9 LCDC (MST=0), CPU-visible
 * BG/OBJ destinations supported by the A-I bank matrix, and C/D to ARM7
 * Work RAM (MST=2). BG extended-palette memory is also readable internally
 * by the display pipeline while remaining inaccessible to guest CPUs.
 */
#ifndef ORACIS_MEMORY_NDS_VRAM_CONTROL_HPP
#define ORACIS_MEMORY_NDS_VRAM_CONTROL_HPP

#include <array>
#include <cstdint>

#include "oracis/memory/guest_memory.hpp"

namespace oracis::memory {

class NdsVramControl final {
public:
    static constexpr uint32_t VramStatAddress = UINT32_C(0x04000240);
    static constexpr uint32_t VramCntAAddress = UINT32_C(0x04000240);
    static constexpr uint32_t VramCntBAddress = UINT32_C(0x04000241);
    static constexpr uint32_t VramCntCAddress = UINT32_C(0x04000242);
    static constexpr uint32_t VramCntDAddress = UINT32_C(0x04000243);
    static constexpr uint32_t VramCntEAddress = UINT32_C(0x04000244);
    static constexpr uint32_t VramCntFAddress = UINT32_C(0x04000245);
    static constexpr uint32_t VramCntGAddress = UINT32_C(0x04000246);
    static constexpr uint32_t VramCntHAddress = UINT32_C(0x04000248);
    static constexpr uint32_t VramCntIAddress = UINT32_C(0x04000249);
    static constexpr uint32_t BankSize = UINT32_C(0x00020000);

    explicit NdsVramControl(GuestMemory &memory) noexcept
        : memory_(memory) {}

    NdsVramControl(const NdsVramControl &) = delete;
    NdsVramControl &operator=(const NdsVramControl &) = delete;

    Status install();

    ReadResult read(NdsCpu cpu,
                    uint32_t address,
                    uint32_t width) const noexcept;

    Status write(NdsCpu cpu,
                 uint32_t address,
                 uint32_t width,
                 uint32_t value) noexcept;

    uint8_t control_c() const noexcept { return controls_[2]; }
    uint8_t control_d() const noexcept { return controls_[3]; }
    uint8_t status() const noexcept;

    /*
     * Read one 16-bit color from an extended BG2/BG3 palette. The palette
     * number selects one of 16 256-color palettes within the BG's 8 KiB slot.
     */
    ReadResult read_extended_bg_palette(
        unsigned engine_index,
        unsigned bg_index,
        uint8_t bg_palette,
        uint8_t color_index) const noexcept;
    GuestMemory &backing_memory() const noexcept { return memory_; }
    bool installed() const noexcept { return installed_; }

private:
    static int bank_index_for_address(uint32_t address) noexcept;
    int arm9_vram_bank_index_for_address(uint32_t address) const noexcept;
    static int arm7_window_index(uint8_t control) noexcept;
    static bool allocated_to_arm7(uint8_t control) noexcept;
    static bool lcdc_enabled(uint8_t control, uint8_t bank_index) noexcept;

    Status apply_controls(
        const std::array<uint8_t, 9> &controls) noexcept;

    GuestMemory &memory_;
    std::array<uint8_t, 9> controls_{};
    bool installed_ = false;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_VRAM_CONTROL_HPP */
