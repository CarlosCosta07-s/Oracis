/*
 * Nintendo DS VRAM C/D bank controller.
 *
 * This partial device implements C/D LCDC direct access for ARM9 (MST=0)
 * and ARM7 Work RAM mapping (MST=2) for the two 128KB banks. Other VRAM
 * destinations remain unmapped until their corresponding devices exist.
 */
#ifndef ORACIS_MEMORY_NDS_VRAM_CONTROL_HPP
#define ORACIS_MEMORY_NDS_VRAM_CONTROL_HPP

#include <cstdint>

#include "oracis/memory/guest_memory.hpp"

namespace oracis::memory {

class NdsVramControl final {
public:
    static constexpr uint32_t VramStatAddress = UINT32_C(0x04000240);
    static constexpr uint32_t VramCntCAddress = UINT32_C(0x04000242);
    static constexpr uint32_t VramCntDAddress = UINT32_C(0x04000243);
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

    uint8_t control_c() const noexcept { return control_c_; }
    uint8_t control_d() const noexcept { return control_d_; }
    uint8_t status() const noexcept;
    GuestMemory &backing_memory() const noexcept { return memory_; }
    bool installed() const noexcept { return installed_; }

private:
    static int arm7_window_index(uint8_t control) noexcept;
    static bool allocated_to_arm7(uint8_t control) noexcept;
    static bool lcdc_enabled(uint8_t control) noexcept;

    Status apply_controls(uint8_t control_c,
                          uint8_t control_d) noexcept;

    GuestMemory &memory_;
    uint8_t control_c_ = 0u;
    uint8_t control_d_ = 0u;
    bool installed_ = false;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_VRAM_CONTROL_HPP */
