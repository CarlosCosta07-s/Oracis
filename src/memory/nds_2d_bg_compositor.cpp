#include "oracis/memory/nds_2d_bg_compositor.hpp"
#include "oracis/memory/nds_2d_affine_bg_fetcher.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace oracis::memory {
namespace {

constexpr uint32_t kEngineABgPaletteBase = UINT32_C(0x05000000);
constexpr uint32_t kEngineBBgPaletteBase = UINT32_C(0x05000400);
constexpr uint8_t kBackgroundOrderBase = UINT8_C(128);
constexpr uint8_t kBackdropOrder = UINT8_C(132);
constexpr unsigned kVisibleWidth = 256u;

struct Candidate {
    uint16_t rgb555 = 0u;
    uint8_t priority = UINT8_C(4);
    uint8_t order = kBackdropOrder;
    Nds2dLayer layer = Nds2dLayer::Backdrop;
    uint8_t object_index = UINT8_C(0xFF);
    bool force_alpha = false;
    uint8_t bitmap_alpha_eva = 0u;
    bool is_three_d = false;
    uint8_t three_d_alpha5 = UINT8_C(31);
};

bool precedes(const Candidate &left, const Candidate &right) noexcept
{
    if (left.priority != right.priority) {
        return left.priority < right.priority;
    }
    return left.order < right.order;
}

Nds2dComposeResult fetch_error(
    unsigned bg_index, Nds2dBgFetchResult result) noexcept
{
    Nds2dComposeResult output{};
    output.background_index = static_cast<uint8_t>(bg_index);
    output.fetch_status = result.status;
    output.memory_status = result.memory_status;

    if (result.status == Nds2dBgFetchStatus::UnsupportedMode) {
        output.status = Nds2dComposeStatus::UnsupportedBackgroundMode;
    } else if (result.status == Nds2dBgFetchStatus::GuestMemoryError) {
        output.status = Nds2dComposeStatus::GuestMemoryError;
    } else {
        output.status = Nds2dComposeStatus::BackgroundFetchFailed;
    }
    return output;
}

Nds2dComposeResult object_error(Nds2dObjFetchResult result) noexcept
{
    Nds2dComposeResult output{};
    output.object_fetch_status = result.status;
    output.memory_status = result.memory_status;
    output.object_index = result.object_index;
    output.status = result.status == Nds2dObjFetchStatus::GuestMemoryError
        ? Nds2dComposeStatus::GuestMemoryError
        : Nds2dComposeStatus::UnsupportedObjectMode;
    return output;
}

} // namespace

Nds2dComposeResult Nds2dBgCompositor::render_scanline(
    unsigned engine_index,
    const NdsDisplayControl &display,
    uint16_t scanline,
    std::array<Nds2dCompositedPixel, 256> &output,
    const Nds3dBg0Scanline *three_d_bg0) const noexcept
{
    if (engine_index > 1u) {
        return {Nds2dComposeStatus::InvalidEngine};
    }
    if (scanline >= NdsDisplayControl::VisibleScanlines) {
        return {Nds2dComposeStatus::InvalidScanline};
    }
    if (!display.installed()) {
        return {Nds2dComposeStatus::DisplayNotInstalled};
    }

    const uint32_t dispcnt = display.dispcnt(engine_index);
    const bool bg0_uses_three_d = engine_index == 0u &&
        (dispcnt & (UINT32_C(1) << 3u)) != 0u &&
        (dispcnt & (UINT32_C(1) << 8u)) != 0u;
    if (bg0_uses_three_d && three_d_bg0 == nullptr) {
        return {Nds2dComposeStatus::ThreeDSourceUnavailable};
    }
    if (bg0_uses_three_d) {
        for (const Nds3dBg0Pixel &pixel : *three_d_bg0) {
            if (pixel.alpha5 > 31u) {
                return {Nds2dComposeStatus::InvalidThreeDAlpha};
            }
        }
    }
    const NdsDisplayControl::WindowBlendState effects =
        display.window_blend(engine_index);

    Nds2dObjFetcher object_fetcher(memory_);
    Nds2dObjScanline object_line{};
    const Nds2dObjFetchResult object_result =
        object_fetcher.render_scanline(
            engine_index, display, scanline, object_line);
    if (object_result.status != Nds2dObjFetchStatus::Ok &&
        object_result.status != Nds2dObjFetchStatus::Disabled) {
        return object_error(object_result);
    }

    Nds2dTextBgFetcher text_fetcher(memory_, vram_control_);
    Nds2dAffineBgFetcher affine_fetcher(memory_, vram_control_);
    std::array<std::array<Nds2dBgPixel, 256>, NdsDisplayControl::BgCount>
        background_lines{};

    for (unsigned bg = 0u; bg < NdsDisplayControl::BgCount; ++bg) {
        if (bg == 0u && bg0_uses_three_d) {
            continue;
        }
        const bool affine_bg =
            Nds2dAffineBgFetcher::is_affine_tile_bg(
                engine_index, bg, dispcnt) ||
            Nds2dAffineBgFetcher::is_extended_bg_layer(
                engine_index, bg, dispcnt);
        const Nds2dBgFetchResult result = affine_bg
            ? affine_fetcher.render_scanline(
                engine_index, bg, display, scanline, background_lines[bg])
            : text_fetcher.render_scanline(
                engine_index, bg, display, scanline, background_lines[bg]);
        if (result.status == Nds2dBgFetchStatus::Ok ||
            result.status == Nds2dBgFetchStatus::Disabled) {
            continue;
        }
        return fetch_error(bg, result);
    }

    const uint32_t palette_base = engine_index == 0u
        ? kEngineABgPaletteBase
        : kEngineBBgPaletteBase;
    const ReadResult backdrop_read = memory_.read(
        NDS_ARM9, palette_base, 2u);
    if (!backdrop_read) {
        Nds2dComposeResult result{};
        result.status = Nds2dComposeStatus::GuestMemoryError;
        result.memory_status = backdrop_read.status;
        return result;
    }
    const uint16_t backdrop_rgb555 =
        static_cast<uint16_t>(backdrop_read.value & UINT32_C(0x7FFF));

    std::array<Nds2dCompositedPixel, 256> next_line{};
    for (unsigned x = 0u; x < kVisibleWidth; ++x) {
        const Nds2dWindowDecision window = Nds2dEffects::evaluate_window(
            dispcnt,
            effects,
            static_cast<uint16_t>(x),
            scanline,
            object_line.object_window_coverage[x]);

        /* Four BGs, one already-resolved OBJ pixel, and the backdrop. */
        std::array<Candidate, NdsDisplayControl::BgCount + 3u> candidates{};
        std::size_t candidate_count = 0u;

        const auto insert_candidate = [&candidates, &candidate_count](
            const Candidate &candidate) noexcept {
            std::size_t position = candidate_count;
            while (position > 0u &&
                   precedes(candidate, candidates[position - 1u])) {
                candidates[position] = candidates[position - 1u];
                --position;
            }
            candidates[position] = candidate;
            ++candidate_count;
        };

        for (unsigned bg = 0u; bg < NdsDisplayControl::BgCount; ++bg) {
            if (bg == 0u && bg0_uses_three_d) {
                if (!Nds2dEffects::layer_visible(window, Nds2dLayer::Bg0)) {
                    continue;
                }
                const uint16_t horizontal_scroll =
                    display.bg_scroll(engine_index, 0u).horizontal;
                const unsigned source_x =
                    (static_cast<unsigned>(x) + horizontal_scroll) & 0x01FFu;
                if (source_x >= kVisibleWidth) {
                    continue;
                }
                const Nds3dBg0Pixel &three_d_pixel =
                    (*three_d_bg0)[source_x];
                if (three_d_pixel.alpha5 == 0u) {
                    continue;
                }
                insert_candidate(Candidate{
                    static_cast<uint16_t>(
                        three_d_pixel.rgb555 & UINT16_C(0x7FFF)),
                    static_cast<uint8_t>(
                        display.bg_control(engine_index, 0u) & UINT16_C(3)),
                    kBackgroundOrderBase,
                    Nds2dLayer::Bg0,
                    UINT8_C(0xFF),
                    false,
                    0u,
                    true,
                    three_d_pixel.alpha5
                });
                continue;
            }

            const Nds2dBgPixel &pixel = background_lines[bg][x];
            if (pixel.transparent) continue;

            const Nds2dLayer layer = static_cast<Nds2dLayer>(bg);
            if (!Nds2dEffects::layer_visible(window, layer)) continue;

            insert_candidate(Candidate{
                pixel.rgb555,
                pixel.priority,
                static_cast<uint8_t>(kBackgroundOrderBase + bg),
                layer,
                UINT8_C(0xFF),
                false
            });
        }

        if (Nds2dEffects::layer_visible(window, Nds2dLayer::Object)) {
            const auto insert_object_pixel =
                [&insert_candidate](const Nds2dObjPixel &object_pixel) noexcept {
                    if (object_pixel.transparent) return;
                    const bool bitmap_alpha =
                        object_pixel.bitmap_alpha_eva != 0u;
                    insert_candidate(Candidate{
                        object_pixel.rgb555,
                        object_pixel.priority,
                        object_pixel.object_index,
                        Nds2dLayer::Object,
                        object_pixel.object_index,
                        object_pixel.semi_transparent || bitmap_alpha,
                        object_pixel.bitmap_alpha_eva
                    });
                };
            insert_object_pixel(object_line.pixels[x]);
            insert_object_pixel(object_line.second_pixels[x]);
        }

        /* Backdrop is below every BG and is never hidden by window masks. */
        insert_candidate(Candidate{
            backdrop_rgb555,
            UINT8_C(4),
            kBackdropOrder,
            Nds2dLayer::Backdrop,
            UINT8_C(0xFF),
            false
        });

        const Candidate &top = candidates[0];
        const bool has_second = candidate_count > 1u;
        const Candidate second = has_second
            ? candidates[1]
            : Candidate{};

        /*
         * A translucent or bitmap OBJ never blends against another OBJ.
         * Select the nearest BG/backdrop behind it that is enabled as a
         * second target. Keep the ordinary next-pixel metadata separately.
         */
        Candidate effect_second = second;
        bool has_effect_second = has_second;
        const uint16_t effect_mode =
            static_cast<uint16_t>((effects.bldcnt >> 6u) & UINT16_C(3));
        const bool obj_alpha_blending = top.layer == Nds2dLayer::Object &&
            (top.force_alpha ||
             (effect_mode == 1u &&
              (effects.bldcnt & UINT16_C(0x0010)) != 0u));
        if (obj_alpha_blending) {
            const uint16_t second_target_mask =
                static_cast<uint16_t>(
                    (effects.bldcnt >> 8u) & UINT16_C(0x003F));
            effect_second = Candidate{};
            has_effect_second = false;

            for (std::size_t index = 1u;
                 index < candidate_count;
                 ++index) {
                const Candidate &candidate = candidates[index];
                if (candidate.layer == Nds2dLayer::Object) continue;
                const uint8_t target_bit = static_cast<uint8_t>(
                    UINT8_C(1) << static_cast<unsigned>(candidate.layer));
                if ((second_target_mask & target_bit) == 0u) continue;
                effect_second = candidate;
                has_effect_second = true;
                break;
            }
        }

        if (top.is_three_d &&
            window.color_effects_enabled &&
            (effects.bldcnt & UINT16_C(0x0001)) != 0u) {
            /*
             * BG0 3D has a hardware-special alpha path: a selected second
             * target immediately behind it can trigger blending even when
             * BLDCNT selects None or brightness. The exact 5-bit-to-EVA/EVB
             * conversion is not sufficiently specified, so do not substitute
             * BLDALPHA or silently output an incorrect colour.
             */
            const uint16_t second_target_mask = static_cast<uint16_t>(
                (effects.bldcnt >> 8u) & UINT16_C(0x003F));
            const uint8_t second_target_bit = static_cast<uint8_t>(
                UINT8_C(1) << static_cast<unsigned>(effect_second.layer));
            const bool eligible_second = has_effect_second &&
                (second_target_mask & second_target_bit) != 0u;
            if (eligible_second && top.three_d_alpha5 != 0u) {
                return {Nds2dComposeStatus::UnsupportedThreeDAlphaBlend};
            }
        }

        next_line[x] = Nds2dCompositedPixel{
            Nds2dEffects::apply_color_effect(
                top.rgb555,
                top.layer,
                effect_second.rgb555,
                effect_second.layer,
                has_effect_second,
                window.color_effects_enabled,
                effects,
                top.force_alpha,
                top.bitmap_alpha_eva),
            top.layer,
            has_effect_second ? effect_second.layer : Nds2dLayer::Backdrop,
            window.region,
            has_effect_second,
            static_cast<uint8_t>(
                top.layer == Nds2dLayer::Object
                    ? top.object_index : UINT8_C(0xFF)),
            top.layer == Nds2dLayer::Object &&
                top.force_alpha && top.bitmap_alpha_eva == 0u,
            static_cast<uint8_t>(
                !obj_alpha_blending &&
                    second.layer == Nds2dLayer::Object
                    ? second.object_index : UINT8_C(0xFF)),
            top.layer == Nds2dLayer::Object && top.bitmap_alpha_eva != 0u,
            top.bitmap_alpha_eva,
            top.is_three_d,
            static_cast<uint8_t>(
                top.is_three_d ? top.three_d_alpha5 : 0u)
        };
    }

    output = next_line;
    return {};
}

Nds2dComposeResult Nds2dBgCompositor::render_scanline(
    unsigned engine_index,
    const NdsDisplayControl &display,
    uint16_t scanline,
    std::array<Nds2dCompositedPixel, 256> &output,
    Nds3dScanlineCache &three_d_cache) const noexcept
{
    if (engine_index > 1u) return {Nds2dComposeStatus::InvalidEngine};
    if (scanline >= NdsDisplayControl::VisibleScanlines) {
        return {Nds2dComposeStatus::InvalidScanline};
    }
    if (!display.installed()) {
        return {Nds2dComposeStatus::DisplayNotInstalled};
    }

    const uint32_t dispcnt = display.dispcnt(engine_index);
    const bool bg0_uses_three_d = engine_index == 0u &&
        (dispcnt & (UINT32_C(1) << 3u)) != 0u &&
        (dispcnt & (UINT32_C(1) << 8u)) != 0u;
    if (!bg0_uses_three_d) {
        return render_scanline(
            engine_index, display, scanline, output,
            static_cast<const Nds3dBg0Scanline *>(nullptr));
    }

    const Nds3dBg0Scanline *cached_line = nullptr;
    if (three_d_cache.peek(scanline, &cached_line) !=
            Nds3dScanlineCacheStatus::Ok ||
        cached_line == nullptr) {
        return {Nds2dComposeStatus::ThreeDSourceUnavailable};
    }

    std::array<Nds2dCompositedPixel, 256> next_line{};
    const Nds2dComposeResult composed = render_scanline(
        engine_index, display, scanline, next_line, cached_line);
    if (composed.status != Nds2dComposeStatus::Ok) return composed;

    if (three_d_cache.consume(scanline) !=
        Nds3dScanlineCacheStatus::Ok) {
        return {Nds2dComposeStatus::ThreeDSourceUnavailable};
    }
    output = next_line;
    return composed;
}

} // namespace oracis::memory
