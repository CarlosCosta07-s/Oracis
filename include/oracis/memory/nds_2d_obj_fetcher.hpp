/*
 * Nintendo DS OBJ/OAM scanline fetcher.
 *
 * Supports non-affine tiled OBJ in 4bpp/8bpp modes, 1D/2D character mapping,
 * flips, palette transparency, mosaic and OBJ-window coverage. Affine and
 * bitmap OBJ modes return explicit unsupported statuses.
 */
#ifndef ORACIS_MEMORY_NDS_2D_OBJ_FETCHER_HPP
#define ORACIS_MEMORY_NDS_2D_OBJ_FETCHER_HPP

#include <array>
#include <cstdint>

#include "oracis/memory/guest_memory.hpp"
#include "oracis/memory/nds_display_control.hpp"

namespace oracis::memory {

enum class Nds2dObjFetchStatus : uint8_t {
    Ok = 0,
    Disabled,
    InvalidEngine,
    InvalidScanline,
    UnsupportedAffine,
    UnsupportedBitmap,
    UnsupportedShape,
    UnsupportedMapping,
    GuestMemoryError
};

struct Nds2dObjPixel {
    uint16_t rgb555 = 0u;
    uint8_t priority = 3u;
    uint8_t object_index = UINT8_C(0xFF);
    bool transparent = true;
    bool semi_transparent = false;
};

struct Nds2dObjScanline {
    std::array<Nds2dObjPixel, 256> pixels{};
    std::array<bool, 256> object_window_coverage{};
};

struct Nds2dObjFetchResult {
    Nds2dObjFetchStatus status = Nds2dObjFetchStatus::Ok;
    Status memory_status = Status::Ok;
    uint8_t object_index = UINT8_C(0xFF);
};

class Nds2dObjFetcher final {
public:
    explicit Nds2dObjFetcher(GuestMemory &memory) noexcept
        : memory_(memory) {}

    Nds2dObjFetcher(const Nds2dObjFetcher &) = delete;
    Nds2dObjFetcher &operator=(const Nds2dObjFetcher &) = delete;

    /*
     * Rasterizes non-affine tiled OBJ entries intersecting one visible line.
     * Output is committed only after all required OAM/VRAM/palette reads.
     */
    Nds2dObjFetchResult render_scanline(
        unsigned engine_index,
        const NdsDisplayControl &display,
        uint16_t scanline,
        Nds2dObjScanline &output) const noexcept;

private:
    GuestMemory &memory_;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_2D_OBJ_FETCHER_HPP */
