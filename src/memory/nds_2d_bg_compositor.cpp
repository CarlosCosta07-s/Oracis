#include "oracis/memory/nds_2d_bg_compositor.hpp"

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
    std::array<Nds2dCompositedPixel, 256> &output) const noexcept
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

    Nds2dTextBgFetcher fetcher(memory_);
    std::array<std::array<Nds2dBgPixel, 256>, NdsDisplayControl::BgCount>
        background_lines{};

    for (unsigned bg = 0u; bg < NdsDisplayControl::BgCount; ++bg) {
        const Nds2dBgFetchResult result = fetcher.render_scanline(
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
        std::array<Candidate, NdsDisplayControl::BgCount + 2u> candidates{};
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

        const Nds2dObjPixel &object_pixel = object_line.pixels[x];
        if (!object_pixel.transparent &&
            Nds2dEffects::layer_visible(window, Nds2dLayer::Object)) {
            insert_candidate(Candidate{
                object_pixel.rgb555,
                object_pixel.priority,
                object_pixel.object_index,
                Nds2dLayer::Object,
                object_pixel.object_index,
                object_pixel.semi_transparent
            });
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

        next_line[x] = Nds2dCompositedPixel{
            Nds2dEffects::apply_color_effect(
                top.rgb555,
                top.layer,
                second.rgb555,
                second.layer,
                has_second,
                window.color_effects_enabled,
                effects,
                top.force_alpha),
            top.layer,
            has_second ? second.layer : Nds2dLayer::Backdrop,
            window.region,
            has_second,
            top.layer == Nds2dLayer::Object
                ? top.object_index : UINT8_C(0xFF),
            top.layer == Nds2dLayer::Object && top.force_alpha
        };
    }

    output = next_line;
    return {};
}

} // namespace oracis::memory
