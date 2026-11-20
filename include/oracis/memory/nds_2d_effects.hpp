/*
 * Nintendo DS 2D window, mosaic and colour-effect evaluation helpers.
 *
 * This unit contains deterministic per-pixel rules consumed by a future
 * scanline compositor. It does not fetch BG/OBJ pixels or render a frame.
 */
#ifndef ORACIS_MEMORY_NDS_2D_EFFECTS_HPP
#define ORACIS_MEMORY_NDS_2D_EFFECTS_HPP

#include <cstdint>

#include "oracis/memory/nds_display_control.hpp"

namespace oracis::memory {

enum class Nds2dLayer : uint8_t {
    Bg0 = 0u,
    Bg1 = 1u,
    Bg2 = 2u,
    Bg3 = 3u,
    Object = 4u,
    Backdrop = 5u
};

enum class Nds2dWindowRegion : uint8_t {
    NoWindows = 0u,
    Window0,
    Window1,
    ObjectWindow,
    Outside
};

struct Nds2dWindowDecision {
    Nds2dWindowRegion region = Nds2dWindowRegion::NoWindows;
    /* Bits 0..4 correspond to BG0..BG3 and OBJ. Backdrop is always available. */
    uint8_t visible_layers = UINT8_C(0x1F);
    bool color_effects_enabled = true;
};

struct Nds2dMosaicSample {
    uint16_t x = 0u;
    uint16_t y = 0u;
    uint8_t horizontal_size = 1u;
    uint8_t vertical_size = 1u;
};

class Nds2dEffects final {
public:
    /*
     * Evaluates one visible LCD pixel (x: 0..255, y: 0..191).
     * OBJ-window coverage is supplied by the future OBJ rasterizer.
     * WIN0 has priority over WIN1, then OBJ-window, then outside.
     */
    static Nds2dWindowDecision evaluate_window(
        uint32_t dispcnt,
        const NdsDisplayControl::WindowBlendState &registers,
        uint16_t x,
        uint16_t y,
        bool object_window_coverage) noexcept;

    static bool layer_visible(
        const Nds2dWindowDecision &window,
        Nds2dLayer layer) noexcept;

    /*
     * Returns the upper-left source coordinate for a BG/OBJ mosaic block.
     * The caller supplies the BGxCNT/OAM mosaic enable flag separately.
     */
    static Nds2dMosaicSample mosaic_sample(
        uint16_t x,
        uint16_t y,
        uint16_t mosaic_register,
        bool object_layer,
        bool mosaic_enabled) noexcept;

    /*
     * Apply one effect to the already-selected top pixel and, for alpha,
     * the next lower non-transparent pixel. RGB inputs/outputs are RGB555.
     * force_alpha_blending is for hardware-special first pixels such as
     * semi-transparent OBJ, bitmap OBJ or a 3D surface on BG0.
     */
    static uint16_t apply_color_effect(
        uint16_t first_rgb555,
        Nds2dLayer first_layer,
        uint16_t second_rgb555,
        Nds2dLayer second_layer,
        bool second_pixel_available,
        bool window_allows_effects,
        const NdsDisplayControl::WindowBlendState &registers,
        bool force_alpha_blending = false) noexcept;

private:
    static bool contains(
        uint16_t horizontal_bounds,
        uint16_t vertical_bounds,
        uint16_t x,
        uint16_t y) noexcept;
    static uint8_t layer_bit(Nds2dLayer layer) noexcept;
    static uint8_t window_layer_mask(
        uint16_t window_register, bool upper_region) noexcept;
    static uint16_t alpha_blend(
        uint16_t first_rgb555,
        uint16_t second_rgb555,
        uint16_t eva,
        uint16_t evb) noexcept;
    static uint16_t change_brightness(
        uint16_t rgb555,
        uint16_t factor,
        bool increase) noexcept;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_2D_EFFECTS_HPP */
