#include "oracis/memory/nds_2d_effects.hpp"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_2d_effects_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using oracis::memory::Nds2dEffects;
    using oracis::memory::Nds2dLayer;
    using oracis::memory::Nds2dWindowRegion;
    using oracis::memory::NdsDisplayControl;

    NdsDisplayControl::WindowBlendState registers{};

    /* WIN0 covers [10,50) x [20,100); WIN1 overlaps with lower priority. */
    registers.bounds[0] = UINT16_C(0x0A32);
    registers.bounds[2] = UINT16_C(0x1464);
    registers.bounds[1] = UINT16_C(0x283C);
    registers.bounds[3] = UINT16_C(0x0A78);
    registers.winin = UINT16_C(0x2231);
    registers.winout = UINT16_C(0x3403);

    constexpr uint32_t all_layers_and_windows =
        (UINT32_C(0x1F) << 8u) | (UINT32_C(7) << 13u);

    const auto win0 = Nds2dEffects::evaluate_window(
        all_layers_and_windows, registers, 20u, 30u, false);
    if (win0.region != Nds2dWindowRegion::Window0 ||
        win0.visible_layers != UINT8_C(0x11) ||
        !win0.color_effects_enabled ||
        !Nds2dEffects::layer_visible(win0, Nds2dLayer::Bg0) ||
        !Nds2dEffects::layer_visible(win0, Nds2dLayer::Object) ||
        Nds2dEffects::layer_visible(win0, Nds2dLayer::Bg1) ||
        !Nds2dEffects::layer_visible(win0, Nds2dLayer::Backdrop)) {
        return fail("WIN0 priority and WININ layer/effect mask failed");
    }

    const auto win1 = Nds2dEffects::evaluate_window(
        all_layers_and_windows, registers, 60u, 30u, false);
    if (win1.region != Nds2dWindowRegion::Window1 ||
        win1.visible_layers != UINT8_C(0x02) ||
        !win1.color_effects_enabled) {
        return fail("WIN1 selection or upper WININ region failed");
    }

    const auto obj_window = Nds2dEffects::evaluate_window(
        all_layers_and_windows, registers, 90u, 130u, true);
    if (obj_window.region != Nds2dWindowRegion::ObjectWindow ||
        obj_window.visible_layers != UINT8_C(0x14) ||
        !obj_window.color_effects_enabled) {
        return fail("OBJ-window coverage or upper WINOUT region failed");
    }

    const auto outside = Nds2dEffects::evaluate_window(
        all_layers_and_windows, registers, 90u, 130u, false);
    if (outside.region != Nds2dWindowRegion::Outside ||
        outside.visible_layers != UINT8_C(0x03) ||
        outside.color_effects_enabled) {
        return fail("outside-window WINOUT mask failed");
    }

    const auto no_windows = Nds2dEffects::evaluate_window(
        UINT32_C(0x00001F00), registers, 200u, 150u, true);
    if (no_windows.region != Nds2dWindowRegion::NoWindows ||
        no_windows.visible_layers != UINT8_C(0x1F) ||
        !no_windows.color_effects_enabled) {
        return fail("disabled windows did not restore master layer visibility");
    }

    const auto master_disable = Nds2dEffects::evaluate_window(
        UINT32_C(0x00001E00) | (UINT32_C(1) << 13u),
        registers, 20u, 30u, false);
    if (master_disable.visible_layers != UINT8_C(0x10)) {
        return fail("DISPCNT master layer disable was not applied after window mask");
    }

    /* X2=0 reaches the right edge (256), while X1=X2=0 disables the window. */
    registers.bounds[0] = UINT16_C(0x0500);
    registers.bounds[2] = UINT16_C(0x000A);
    if (!Nds2dEffects::evaluate_window(
            all_layers_and_windows, registers, 255u, 5u, false).visible_layers ||
        Nds2dEffects::evaluate_window(
            all_layers_and_windows, registers, 4u, 5u, false).region ==
            Nds2dWindowRegion::Window0) {
        return fail("DS X2=0 right-edge window convention failed");
    }
    registers.bounds[0] = 0u;
    if (Nds2dEffects::evaluate_window(
            all_layers_and_windows, registers, 100u, 5u, false).region ==
        Nds2dWindowRegion::Window0) {
        return fail("X1=X2=0 unexpectedly enabled a full-width window");
    }

    /* Original DS VCOUNT low-byte window bug: Y1 in 1..6 acts as zero. */
    registers.bounds[0] = UINT16_C(0x0A32);
    registers.bounds[2] = UINT16_C(0x0414);
    const auto y_bug = Nds2dEffects::evaluate_window(
        all_layers_and_windows, registers, 20u, 0u, false);
    if (y_bug.region != Nds2dWindowRegion::Window0) {
        return fail("DS Y1=0..6 vertical window quirk was not modelled");
    }

    /* A master-disabled window is still selected, but all BG/OBJ are masked. */
    const auto empty_win = Nds2dEffects::evaluate_window(
        UINT32_C(1) << 13u, registers, 20u, 0u, false);
    if (empty_win.region != Nds2dWindowRegion::Window0 ||
        empty_win.visible_layers != 0u) {
        return fail("window region selection incorrectly bypassed layer master enables");
    }

    const auto bg_mosaic = Nds2dEffects::mosaic_sample(
        6u, 5u, UINT16_C(0x0023), false, true);
    if (bg_mosaic.x != 4u || bg_mosaic.y != 4u ||
        bg_mosaic.horizontal_size != 4u ||
        bg_mosaic.vertical_size != 3u) {
        return fail("BG mosaic source-coordinate quantization failed");
    }
    const auto obj_mosaic = Nds2dEffects::mosaic_sample(
        5u, 7u, UINT16_C(0x3100), true, true);
    if (obj_mosaic.x != 4u || obj_mosaic.y != 4u ||
        obj_mosaic.horizontal_size != 2u ||
        obj_mosaic.vertical_size != 4u) {
        return fail("OBJ mosaic source-coordinate quantization failed");
    }
    const auto mosaic_off = Nds2dEffects::mosaic_sample(
        5u, 7u, UINT16_C(0xFFFF), true, false);
    if (mosaic_off.x != 5u || mosaic_off.y != 7u ||
        mosaic_off.horizontal_size != 1u || mosaic_off.vertical_size != 1u) {
        return fail("disabled mosaic changed the sampling coordinate");
    }

    constexpr uint16_t first_rgb = UINT16_C(0x1094);
    constexpr uint16_t second_rgb = UINT16_C(0x7A6A);
    registers.bldcnt =
        static_cast<uint16_t>((1u << 6u) | (1u << 0u) | (1u << 9u));
    registers.bldalpha = UINT16_C(0x0808);
    const uint16_t alpha = Nds2dEffects::apply_color_effect(
        first_rgb, Nds2dLayer::Bg0,
        second_rgb, Nds2dLayer::Bg1,
        true, true, registers);
    if (alpha != UINT16_C(0x44FF)) {
        return fail("RGB555 alpha blend or round-to-nearest result was incorrect");
    }

    const uint16_t no_second = Nds2dEffects::apply_color_effect(
        first_rgb, Nds2dLayer::Bg0,
        second_rgb, Nds2dLayer::Bg1,
        false, true, registers);
    if (no_second != first_rgb) {
        return fail("alpha blending occurred without a second visible pixel");
    }

    const uint16_t window_blocked = Nds2dEffects::apply_color_effect(
        first_rgb, Nds2dLayer::Bg0,
        second_rgb, Nds2dLayer::Bg1,
        true, false, registers);
    if (window_blocked != first_rgb) {
        return fail("window color-effect disable bit did not block alpha blending");
    }

    /* Forced alpha bypasses first-target and mode bits, but still needs a 2nd target. */
    registers.bldcnt = static_cast<uint16_t>(1u << 9u);
    const uint16_t forced_alpha = Nds2dEffects::apply_color_effect(
        first_rgb, Nds2dLayer::Object,
        second_rgb, Nds2dLayer::Bg1,
        true, true, registers, true);
    if (forced_alpha != UINT16_C(0x44FF)) {
        return fail("forced alpha blending did not override BLDCNT mode/first-target selection");
    }

    registers.bldcnt = static_cast<uint16_t>((2u << 6u) | (1u << 0u));
    registers.bldy = 8u;
    const uint16_t bright = Nds2dEffects::apply_color_effect(
        first_rgb, Nds2dLayer::Bg0,
        0u, Nds2dLayer::Backdrop,
        false, true, registers);
    if (bright != UINT16_C(0x34B8)) {
        return fail("brightness-up RGB555 calculation was incorrect");
    }

    registers.bldcnt = static_cast<uint16_t>((3u << 6u) | (1u << 0u));
    const uint16_t dark = Nds2dEffects::apply_color_effect(
        first_rgb, Nds2dLayer::Bg0,
        0u, Nds2dLayer::Backdrop,
        false, true, registers);
    if (dark != UINT16_C(0x084A)) {
        return fail("brightness-down RGB555 calculation was incorrect");
    }

    /* Coefficients greater than 16 are interpreted as 16/16. */
    registers.bldcnt = static_cast<uint16_t>((1u << 6u) | (1u << 5u) | (1u << 8u));
    registers.bldalpha = UINT16_C(0x1F1F);
    const uint16_t clamped = Nds2dEffects::apply_color_effect(
        first_rgb, Nds2dLayer::Backdrop,
        second_rgb, Nds2dLayer::Bg0,
        true, true, registers);
    if (clamped != first_rgb) {
        return fail("alpha coefficient clamp or target selection was not respected");
    }

    std::puts("NDS 2D windows, mosaic and RGB555 color-effect helpers passed");
    return 0;
}
