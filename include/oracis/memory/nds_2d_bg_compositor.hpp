/*
 * Nintendo DS 2D background scanline compositor.
 *
 * Combines text and affine BG fetch results with OBJ pixels using DISPCNT
 * window masks, BG priority/tie ordering, the backdrop palette entry, and
 * the colour-effect evaluator. Bitmap OBJ alpha is blended with a selected
 * BG target; a second OBJ is never used as an alpha target.
 */
#ifndef ORACIS_MEMORY_NDS_2D_BG_COMPOSITOR_HPP
#define ORACIS_MEMORY_NDS_2D_BG_COMPOSITOR_HPP

#include <array>
#include <cstdint>

#include "oracis/memory/nds_2d_bg_fetcher.hpp"
#include "oracis/memory/nds_2d_effects.hpp"
#include "oracis/memory/nds_2d_obj_fetcher.hpp"
#include "oracis/memory/nds_vram_control.hpp"

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
    ObjectFetchFailed,
    ThreeDSourceUnavailable,
    InvalidThreeDAlpha,
    UnsupportedThreeDAlphaBlend
};

struct Nds2dCompositedPixel {
    uint16_t rgb555 = 0u;
    Nds2dLayer top_layer = Nds2dLayer::Backdrop;
    Nds2dLayer second_layer = Nds2dLayer::Backdrop;
    Nds2dWindowRegion window_region = Nds2dWindowRegion::NoWindows;
    bool second_pixel_available = false;
    uint8_t top_object_index = UINT8_C(0xFF);
    bool top_object_semi_transparent = false;
    uint8_t second_object_index = UINT8_C(0xFF);
    bool top_object_bitmap = false;
    uint8_t top_object_bitmap_alpha_eva = 0u;
    bool top_layer_is_3d = false;
    uint8_t top_3d_alpha5 = 0u;
};

/*
 * One pixel supplied by the DS 3D renderer for Engine A's BG0 input.
 * RGB is RGB555; alpha is the native 5-bit 3D alpha value. Alpha zero is
 * transparent at the 2D-engine boundary.
 */
struct Nds3dBg0Pixel {
    uint16_t rgb555 = 0u;
    uint8_t alpha5 = 0u;
};

using Nds3dBg0Scanline = std::array<Nds3dBg0Pixel, 256>;

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
    explicit Nds2dBgCompositor(
        GuestMemory &memory,
        NdsVramControl *vram_control = nullptr) noexcept
        : memory_(memory), vram_control_(vram_control) {}

    Nds2dBgCompositor(const Nds2dBgCompositor &) = delete;
    Nds2dBgCompositor &operator=(const Nds2dBgCompositor &) = delete;

    /*
     * Compose one visible 256-pixel line. The caller's output is unchanged
     * unless all required BG, OBJ/OAM, BG0 3D source and backdrop reads
     * succeed. When Engine A selects 3D for enabled BG0 (DISPCNT bit 3), the
     * caller must supply that rendered 3D scanline. BG0HOFS is applied across
     * the 512-pixel horizontal wrap region; alpha zero is transparent.
     * Partial-alpha 3D first-target blending currently fails closed because
     * the exact hardware coefficient path is not yet verified.
     */
    Nds2dComposeResult render_scanline(
        unsigned engine_index,
        const NdsDisplayControl &display,
        uint16_t scanline,
        std::array<Nds2dCompositedPixel, 256> &output,
        const Nds3dBg0Scanline *three_d_bg0 = nullptr) const noexcept;

private:
    GuestMemory &memory_;
    NdsVramControl *vram_control_ = nullptr;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_2D_BG_COMPOSITOR_HPP */
