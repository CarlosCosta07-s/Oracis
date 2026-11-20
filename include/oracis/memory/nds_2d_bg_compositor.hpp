/*
 * Nintendo DS 2D background scanline compositor.
 *
 * Combines the four text-BG fetch results using DISPCNT window masks,
 * BG priority/tie ordering, the backdrop palette entry, and the existing
 * colour-effect evaluator. OBJ and non-text BG sources remain explicit
 * unsupported inputs until their fetchers/rasterizers are available.
 */
#ifndef ORACIS_MEMORY_NDS_2D_BG_COMPOSITOR_HPP
#define ORACIS_MEMORY_NDS_2D_BG_COMPOSITOR_HPP

#include <array>
#include <cstdint>

#include "oracis/memory/nds_2d_bg_fetcher.hpp"
#include "oracis/memory/nds_2d_effects.hpp"
#include "oracis/memory/nds_2d_obj_fetcher.hpp"

namespace oracis::memory {

enum class Nds2dComposeStatus : uint8_t {
    Ok = 0,
    InvalidEngine,
    InvalidScanline,
    DisplayNotInstalled,
    UnsupportedBackgroundMode,
    GuestMemoryError,
    BackgroundFetchFailed,
    UnsupportedObjectMode,
    ObjectFetchFailed
};

struct Nds2dCompositedPixel {
    uint16_t rgb555 = 0u;
    Nds2dLayer top_layer = Nds2dLayer::Backdrop;
    Nds2dLayer second_layer = Nds2dLayer::Backdrop;
    Nds2dWindowRegion window_region = Nds2dWindowRegion::NoWindows;
    bool second_pixel_available = false;
    uint8_t top_object_index = UINT8_C(0xFF);
    bool top_object_semi_transparent = false;
};

struct Nds2dComposeResult {
    Nds2dComposeStatus status = Nds2dComposeStatus::Ok;
    uint8_t background_index = UINT8_C(0xFF);
    Nds2dBgFetchStatus fetch_status = Nds2dBgFetchStatus::Ok;
    Status memory_status = Status::Ok;
    Nds2dObjFetchStatus object_fetch_status = Nds2dObjFetchStatus::Ok;
    uint8_t object_index = UINT8_C(0xFF);
};

class Nds2dBgCompositor final {
public:
    explicit Nds2dBgCompositor(GuestMemory &memory) noexcept
        : memory_(memory) {}

    Nds2dBgCompositor(const Nds2dBgCompositor &) = delete;
    Nds2dBgCompositor &operator=(const Nds2dBgCompositor &) = delete;

    /*
     * Compose one visible 256-pixel line. The caller's output is unchanged
     * unless all required BG, OBJ/OAM and backdrop reads succeed. Unsupported
     * affine/bitmap BG or OBJ inputs return a diagnostic without partial output.
     */
    Nds2dComposeResult render_scanline(
        unsigned engine_index,
        const NdsDisplayControl &display,
        uint16_t scanline,
        std::array<Nds2dCompositedPixel, 256> &output) const noexcept;

private:
    GuestMemory &memory_;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_2D_BG_COMPOSITOR_HPP */
