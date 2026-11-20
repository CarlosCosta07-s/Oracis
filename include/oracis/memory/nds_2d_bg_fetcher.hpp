/*
 * Nintendo DS 2D text-background tile/map fetcher.
 *
 * Fetches one 256-pixel scanline from a text BG using the live NDS display
 * registers, mapped BG VRAM, and standard BG palette RAM. Affine/bitmap BG,
 * OBJ evaluation, layer priority, windows and final composition are handled
 * by later stages.
 */
#ifndef ORACIS_MEMORY_NDS_2D_BG_FETCHER_HPP
#define ORACIS_MEMORY_NDS_2D_BG_FETCHER_HPP

#include <array>
#include <cstdint>

#include "oracis/memory/guest_memory.hpp"
#include "oracis/memory/nds_display_control.hpp"
#include "oracis/memory/nds_vram_control.hpp"

namespace oracis::memory {

enum class Nds2dBgFetchStatus : uint8_t {
    Ok = 0,
    Disabled,
    DisplayNotInstalled,
    InvalidEngine,
    InvalidBackground,
    InvalidScanline,
    UnsupportedMode,
    GuestMemoryError
};

struct Nds2dBgFetchResult {
    Nds2dBgFetchStatus status = Nds2dBgFetchStatus::Ok;
    Status memory_status = Status::Ok;
};

struct Nds2dBgPixel {
    uint16_t rgb555 = 0u;
    uint8_t priority = 3u;
    uint8_t bg_index = 0u;
    bool transparent = true;
};

class Nds2dTextBgFetcher final {
public:
    explicit Nds2dTextBgFetcher(
        GuestMemory &memory,
        NdsVramControl *vram_control = nullptr) noexcept
        : memory_(memory), vram_control_(vram_control) {}

    Nds2dTextBgFetcher(const Nds2dTextBgFetcher &) = delete;
    Nds2dTextBgFetcher &operator=(const Nds2dTextBgFetcher &) = delete;

    /*
     * Fetches a complete visible scanline into a temporary buffer and commits
     * only on success. Standard BG VRAM/palettes use the ARM9 guest map;
     * extended palettes use the PPU-only VRAM controller interface.
     */
    Nds2dBgFetchResult render_scanline(
        unsigned engine_index,
        unsigned bg_index,
        const NdsDisplayControl &display,
        uint16_t scanline,
        std::array<Nds2dBgPixel, 256> &output) const noexcept;

private:
    static bool text_mode_supported(
        unsigned engine_index,
        unsigned bg_index,
        uint32_t dispcnt) noexcept;

    static bool add_guest_offset(
        uint32_t base, uint64_t offset, uint32_t &address) noexcept;

    GuestMemory &memory_;
    NdsVramControl *vram_control_ = nullptr;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_2D_BG_FETCHER_HPP */
