#include "oracis/memory/nds_2d_effects.hpp"

#include <algorithm>

namespace oracis::memory {
namespace {

constexpr uint32_t kWindow0Enable = UINT32_C(1) << 13u;
constexpr uint32_t kWindow1Enable = UINT32_C(1) << 14u;
constexpr uint32_t kObjectWindowEnable = UINT32_C(1) << 15u;
constexpr uint32_t kMasterLayerShift = 8u;
constexpr uint16_t kRgb555Mask = UINT16_C(0x7FFF);
constexpr uint16_t kFiveBitMask = UINT16_C(0x001F);
constexpr uint16_t kAlphaMode = 1u;
constexpr uint16_t kBrightnessUpMode = 2u;
constexpr uint16_t kBrightnessDownMode = 3u;

uint8_t component(uint16_t rgb555, unsigned shift) noexcept
{
    return static_cast<uint8_t>((rgb555 >> shift) & kFiveBitMask);
}

uint16_t pack_rgb555(uint8_t red, uint8_t green, uint8_t blue) noexcept
{
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(red) & kFiveBitMask) |
        ((static_cast<uint16_t>(green) & kFiveBitMask) << 5u) |
        ((static_cast<uint16_t>(blue) & kFiveBitMask) << 10u));
}

uint8_t rounded_sixteenth(uint32_t numerator) noexcept
{
    return static_cast<uint8_t>(std::min<uint32_t>(31u, (numerator + 8u) / 16u));
}

} // namespace

bool Nds2dEffects::contains(
    uint16_t horizontal_bounds,
    uint16_t vertical_bounds,
    uint16_t x,
    uint16_t y) noexcept
{
    if (x >= 256u || y >= 192u) return false;

    const uint16_t left = static_cast<uint16_t>(horizontal_bounds >> 8u);
    const uint16_t right_byte =
        static_cast<uint16_t>(horizontal_bounds & UINT16_C(0x00FF));
    const uint16_t right_exclusive =
        right_byte == 0u ? UINT16_C(256) : right_byte;

    /* X1=X2=0 is the disabled-window encoding on original NDS hardware. */
    if (left == 0u && right_byte == 0u) return false;
    if (left >= right_exclusive) return false;

    uint16_t top = static_cast<uint16_t>(vertical_bounds >> 8u);
    const uint16_t bottom_exclusive =
        static_cast<uint16_t>(vertical_bounds & UINT16_C(0x00FF));

    /*
     * Original DS 2D engines compare VCOUNT's low byte. Y1 values 1..6 can
     * remain asserted over frame wrap, making their visible-frame start 0.
     */
    if (top <= 6u) top = 0u;
    if (top >= bottom_exclusive) return false;

    return x >= left && x < right_exclusive &&
           y >= top && y < bottom_exclusive;
}

uint8_t Nds2dEffects::layer_bit(Nds2dLayer layer) noexcept
{
    const unsigned index = static_cast<unsigned>(layer);
    if (index > static_cast<unsigned>(Nds2dLayer::Backdrop)) return 0u;
    return static_cast<uint8_t>(UINT8_C(1) << index);
}

uint8_t Nds2dEffects::window_layer_mask(
    uint16_t window_register, bool upper_region) noexcept
{
    const uint16_t region = upper_region
        ? static_cast<uint16_t>(window_register >> 8u)
        : window_register;
    return static_cast<uint8_t>(region & UINT16_C(0x003F));
}

Nds2dWindowDecision Nds2dEffects::evaluate_window(
    uint32_t dispcnt,
    const NdsDisplayControl::WindowBlendState &registers,
    uint16_t x,
    uint16_t y,
    bool object_window_coverage) noexcept
{
    const uint8_t master_layers =
        static_cast<uint8_t>((dispcnt >> kMasterLayerShift) & UINT32_C(0x1F));

    const bool window0_enabled = (dispcnt & kWindow0Enable) != 0u;
    const bool window1_enabled = (dispcnt & kWindow1Enable) != 0u;
    const bool object_window_enabled = (dispcnt & kObjectWindowEnable) != 0u;

    if (!window0_enabled && !window1_enabled && !object_window_enabled) {
        return {Nds2dWindowRegion::NoWindows, master_layers, true};
    }

    uint16_t region_control = 0u;
    Nds2dWindowRegion region = Nds2dWindowRegion::Outside;

    if (window0_enabled &&
        contains(registers.bounds[0], registers.bounds[2], x, y)) {
        region = Nds2dWindowRegion::Window0;
        region_control = registers.winin;
    } else if (window1_enabled &&
               contains(registers.bounds[1], registers.bounds[3], x, y)) {
        region = Nds2dWindowRegion::Window1;
        region_control = static_cast<uint16_t>(registers.winin >> 8u);
    } else if (object_window_enabled && object_window_coverage) {
        region = Nds2dWindowRegion::ObjectWindow;
        region_control = static_cast<uint16_t>(registers.winout >> 8u);
    } else {
        region = Nds2dWindowRegion::Outside;
        region_control = registers.winout;
    }

    const uint8_t window_layers =
        static_cast<uint8_t>(region_control & UINT16_C(0x001F));
    const bool effects_enabled =
        (region_control & UINT16_C(0x0020)) != 0u;

    return {
        region,
        static_cast<uint8_t>(master_layers & window_layers),
        effects_enabled
    };
}

bool Nds2dEffects::layer_visible(
    const Nds2dWindowDecision &window, Nds2dLayer layer) noexcept
{
    if (layer == Nds2dLayer::Backdrop) return true;
    const uint8_t bit = layer_bit(layer);
    return bit != 0u && (window.visible_layers & bit) != 0u;
}

Nds2dMosaicSample Nds2dEffects::mosaic_sample(
    uint16_t x,
    uint16_t y,
    uint16_t mosaic_register,
    bool object_layer,
    bool mosaic_enabled) noexcept
{
    if (!mosaic_enabled) return {x, y, 1u, 1u};

    const unsigned shift = object_layer ? 8u : 0u;
    const uint8_t horizontal = static_cast<uint8_t>(
        ((mosaic_register >> shift) & UINT16_C(0x000F)) + 1u);
    const uint8_t vertical = static_cast<uint8_t>(
        ((mosaic_register >> (shift + 4u)) & UINT16_C(0x000F)) + 1u);

    return {
        static_cast<uint16_t>(x - (x % horizontal)),
        static_cast<uint16_t>(y - (y % vertical)),
        horizontal,
        vertical
    };
}

uint16_t Nds2dEffects::alpha_blend(
    uint16_t first_rgb555,
    uint16_t second_rgb555,
    uint16_t eva,
    uint16_t evb) noexcept
{
    eva = std::min<uint16_t>(eva, 16u);
    evb = std::min<uint16_t>(evb, 16u);

    const uint8_t red = rounded_sixteenth(
        component(first_rgb555, 0u) * eva +
        component(second_rgb555, 0u) * evb);
    const uint8_t green = rounded_sixteenth(
        component(first_rgb555, 5u) * eva +
        component(second_rgb555, 5u) * evb);
    const uint8_t blue = rounded_sixteenth(
        component(first_rgb555, 10u) * eva +
        component(second_rgb555, 10u) * evb);
    return pack_rgb555(red, green, blue);
}

uint16_t Nds2dEffects::change_brightness(
    uint16_t rgb555, uint16_t factor, bool increase) noexcept
{
    factor = std::min<uint16_t>(factor, 16u);
    const auto adjust = [factor, increase](uint8_t value) noexcept {
        const uint32_t amount = increase
            ? (static_cast<uint32_t>(31u - value) * factor)
            : (static_cast<uint32_t>(value) * factor);
        const uint8_t rounded = rounded_sixteenth(amount);
        return increase
            ? static_cast<uint8_t>(std::min<unsigned>(31u, value + rounded))
            : static_cast<uint8_t>(value - rounded);
    };

    return pack_rgb555(
        adjust(component(rgb555, 0u)),
        adjust(component(rgb555, 5u)),
        adjust(component(rgb555, 10u)));
}

uint16_t Nds2dEffects::apply_color_effect(
    uint16_t first_rgb555,
    Nds2dLayer first_layer,
    uint16_t second_rgb555,
    Nds2dLayer second_layer,
    bool second_pixel_available,
    bool window_allows_effects,
    const NdsDisplayControl::WindowBlendState &registers,
    bool force_alpha_blending) noexcept
{
    const uint16_t first = first_rgb555 & kRgb555Mask;
    const uint16_t second = second_rgb555 & kRgb555Mask;
    if (!window_allows_effects) return first;

    const uint16_t bldcnt =
        registers.bldcnt & NdsDisplayControl::BlendControlMask;
    const uint8_t first_target = layer_bit(first_layer);
    const uint8_t second_target = layer_bit(second_layer);
    const bool is_first_target =
        first_target != 0u && (bldcnt & first_target) != 0u;
    const bool is_second_target =
        second_target != 0u &&
        (bldcnt & static_cast<uint16_t>(second_target << 8u)) != 0u &&
        second_pixel_available;

    const uint16_t effect_mode =
        static_cast<uint16_t>((bldcnt >> 6u) & UINT16_C(0x0003));
    const bool alpha_requested = force_alpha_blending ||
        (is_first_target && effect_mode == kAlphaMode);

    if (alpha_requested) {
        if (!is_second_target) return first;
        const uint16_t eva =
            static_cast<uint16_t>(registers.bldalpha & UINT16_C(0x001F));
        const uint16_t evb =
            static_cast<uint16_t>((registers.bldalpha >> 8u) & UINT16_C(0x001F));
        return alpha_blend(first, second, eva, evb);
    }

    if (!is_first_target) return first;

    const uint16_t factor =
        static_cast<uint16_t>(registers.bldy & NdsDisplayControl::BlendYMask);
    if (effect_mode == kBrightnessUpMode) {
        return change_brightness(first, factor, true);
    }
    if (effect_mode == kBrightnessDownMode) {
        return change_brightness(first, factor, false);
    }
    return first;
}

} // namespace oracis::memory
