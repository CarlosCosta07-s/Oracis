/*
 * Nintendo DS classic affine tiled-background fetcher (BG2/BG3).
 *
 * Supports BG Modes 1/2/4 affine tile layers only. Bitmap and extended
 * backgrounds remain explicit unsupported inputs.
 */
#ifndef ORACIS_MEMORY_NDS_2D_AFFINE_BG_FETCHER_HPP
#define ORACIS_MEMORY_NDS_2D_AFFINE_BG_FETCHER_HPP

#include <array>
#include <cstdint>

#include "oracis/memory/nds_2d_bg_fetcher.hpp"

namespace oracis::memory {

class Nds2dAffineBgFetcher final {
public:
    explicit Nds2dAffineBgFetcher(GuestMemory &memory) noexcept
        : memory_(memory) {}

    Nds2dAffineBgFetcher(const Nds2dAffineBgFetcher &) = delete;
    Nds2dAffineBgFetcher &operator=(const Nds2dAffineBgFetcher &) = delete;

    static bool is_affine_tile_bg(
        unsigned engine_index,
        unsigned bg_index,
        uint32_t dispcnt) noexcept;

    /*
     * Fetches one affine 8bpp tile-map scanline. BGxX/BGxY are signed 19.8
     * reference points, with PB/PD scanline accumulation from the display
     * device. Output commits only on success.
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
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_2D_AFFINE_BG_FETCHER_HPP */
