/*
 * Nintendo DS affine-background fetcher (BG2/BG3).
 *
 * Supports classic affine tile layers in Modes 1/2/4 and extended affine
 * BG layers in Modes 3/4/5, including tile maps and indexed/direct-color
 * bitmaps. Extended tile-map palettes remain explicitly unsupported.
 */
#ifndef ORACIS_MEMORY_NDS_2D_AFFINE_BG_FETCHER_HPP
#define ORACIS_MEMORY_NDS_2D_AFFINE_BG_FETCHER_HPP

#include <array>
#include <cstdint>

#include "oracis/memory/nds_2d_bg_fetcher.hpp"
#include "oracis/memory/nds_vram_control.hpp"

namespace oracis::memory {

class Nds2dAffineBgFetcher final {
public:
    explicit Nds2dAffineBgFetcher(
        GuestMemory &memory,
        NdsVramControl *vram_control = nullptr) noexcept
        : memory_(memory), vram_control_(vram_control) {}

    Nds2dAffineBgFetcher(const Nds2dAffineBgFetcher &) = delete;
    Nds2dAffineBgFetcher &operator=(const Nds2dAffineBgFetcher &) = delete;

    static bool is_affine_tile_bg(
        unsigned engine_index,
        unsigned bg_index,
        uint32_t dispcnt) noexcept;

    static bool is_extended_bg_layer(
        unsigned engine_index,
        unsigned bg_index,
        uint32_t dispcnt) noexcept;

    /*
     * Fetches one affine BG scanline. Classic maps use one-byte entries;
     * extended maps use 16-bit tile/flip entries; bitmap variants use
     * indexed 8bpp or direct-color 16bpp pixels. BGxX/BGxY are signed 19.8
     * references with PB/PD scanline accumulation. Output commits on success.
     */
    Nds2dBgFetchResult render_scanline(
        unsigned engine_index,
        unsigned bg_index,
        const NdsDisplayControl &display,
        uint16_t scanline,
        std::array<Nds2dBgPixel, 256> &output) const noexcept;

private:
    static bool add_guest_offset(
        uint32_t base, uint64_t offset, uint32_t &address) noexcept;

    GuestMemory &memory_;
    NdsVramControl *vram_control_ = nullptr;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_2D_AFFINE_BG_FETCHER_HPP */
