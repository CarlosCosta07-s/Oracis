#include "oracis/memory/nds_2d_bg_fetcher.hpp"

#include <cstdint>

#include "oracis/memory/nds_2d_effects.hpp"

namespace oracis::memory {
namespace {

constexpr uint32_t kEngineABgVramBase = UINT32_C(0x06000000);
constexpr uint32_t kEngineBBgVramBase = UINT32_C(0x06200000);
constexpr uint32_t kEngineABgPaletteBase = UINT32_C(0x05000000);
constexpr uint32_t kEngineBBgPaletteBase = UINT32_C(0x05000400);
constexpr uint32_t kBgEnableShift = 8u;
constexpr uint32_t kEngineABg0Is3d = UINT32_C(1) << 3u;
constexpr uint32_t kExtendedBgPaletteEnable = UINT32_C(1) << 30u;
constexpr uint32_t kScreenBlockBytes = UINT32_C(0x0800);
constexpr uint32_t kScreenBlockTiles = 32u;
constexpr uint32_t kPixelsPerTile = 8u;

Nds2dBgFetchResult memory_error(Status status) noexcept
{
    return {Nds2dBgFetchStatus::GuestMemoryError, status};
}

} // namespace

bool Nds2dTextBgFetcher::add_guest_offset(
    uint32_t base, uint64_t offset, uint32_t &address) noexcept
{
    const uint64_t result = static_cast<uint64_t>(base) + offset;
    if (result > UINT32_MAX) return false;
    address = static_cast<uint32_t>(result);
    return true;
}

bool Nds2dTextBgFetcher::text_mode_supported(
    unsigned engine_index,
    unsigned bg_index,
    uint32_t dispcnt) noexcept
{
    if (engine_index > 1u || bg_index >= NdsDisplayControl::BgCount) {
        return false;
    }

    const unsigned mode = static_cast<unsigned>(dispcnt & UINT32_C(7));
    if (mode > 5u) return false;

    /* Engine A can route BG0 to the 3D FIFO instead of the 2D tile fetcher. */
    if (engine_index == 0u && bg_index == 0u &&
        (dispcnt & kEngineABg0Is3d) != 0u) {
        return false;
    }

    /*
     * DS modes:
     * 0: BG0-3 text; 1: BG0-2 text; 2: BG0-1 text;
     * 3: BG0-2 text; 4-5: BG0-1 text.
     * Remaining enabled BGs use affine/extended/bitmap inputs.
     */
    switch (mode) {
    case 0u:
        return true;
    case 1u:
    case 3u:
        return bg_index <= 2u;
    case 2u:
    case 4u:
    case 5u:
        return bg_index <= 1u;
    default:
        return false;
    }
}

Nds2dBgFetchResult Nds2dTextBgFetcher::render_scanline(
    unsigned engine_index,
    unsigned bg_index,
    const NdsDisplayControl &display,
    uint16_t scanline,
    std::array<Nds2dBgPixel, 256> &output) const noexcept
{
    if (engine_index > 1u) {
        return {Nds2dBgFetchStatus::InvalidEngine, Status::InvalidCpu};
    }
    if (bg_index >= NdsDisplayControl::BgCount) {
        return {Nds2dBgFetchStatus::InvalidBackground, Status::InvalidRegion};
    }
    if (scanline >= 192u) {
        return {Nds2dBgFetchStatus::InvalidScanline, Status::InvalidRegion};
    }
    if (!display.installed()) {
        return {Nds2dBgFetchStatus::DisplayNotInstalled, Status::Unmapped};
    }

    const uint32_t dispcnt = display.dispcnt(engine_index);
    const uint16_t bgcnt = display.bg_control(engine_index, bg_index);
    const uint32_t layer_enable_bit =
        UINT32_C(1) << (kBgEnableShift + bg_index);

    std::array<Nds2dBgPixel, 256> next_line{};
    for (Nds2dBgPixel &pixel : next_line) {
        pixel.priority = static_cast<uint8_t>(bgcnt & UINT16_C(3));
        pixel.bg_index = static_cast<uint8_t>(bg_index);
    }

    if ((dispcnt & layer_enable_bit) == 0u) {
        output = next_line;
        return {Nds2dBgFetchStatus::Disabled, Status::Ok};
    }

    if (!text_mode_supported(engine_index, bg_index, dispcnt) ||
        (dispcnt & kExtendedBgPaletteEnable) != 0u) {
        return {Nds2dBgFetchStatus::UnsupportedMode, Status::Ok};
    }

    const uint32_t bg_vram_base =
        engine_index == 0u ? kEngineABgVramBase : kEngineBBgVramBase;
    const uint32_t bg_palette_base =
        engine_index == 0u ? kEngineABgPaletteBase : kEngineBBgPaletteBase;

    /* Character and screen offsets are present only in the main engine. */
    const uint32_t character_base_block =
        static_cast<uint32_t>((bgcnt >> 2u) & UINT16_C(0x000F));
    const uint32_t screen_base_block =
        static_cast<uint32_t>((bgcnt >> 8u) & UINT16_C(0x001F));
    const uint32_t character_base_offset =
        engine_index == 0u
            ? ((dispcnt >> 24u) & UINT32_C(7)) * UINT32_C(0x10000)
            : 0u;
    const uint32_t screen_base_offset =
        engine_index == 0u
            ? ((dispcnt >> 27u) & UINT32_C(7)) * UINT32_C(0x10000)
            : 0u;
    const uint64_t character_base =
        static_cast<uint64_t>(character_base_offset) +
        static_cast<uint64_t>(character_base_block) * UINT32_C(0x4000);
    const uint64_t screen_base =
        static_cast<uint64_t>(screen_base_offset) +
        static_cast<uint64_t>(screen_base_block) * kScreenBlockBytes;

    const unsigned screen_size =
        static_cast<unsigned>((bgcnt >> 14u) & UINT16_C(3));
    const uint16_t screen_width =
        (screen_size & 1u) != 0u ? 512u : 256u;
    const uint16_t screen_height =
        (screen_size & 2u) != 0u ? 512u : 256u;
    const uint16_t width_tiles =
        static_cast<uint16_t>(screen_width / kPixelsPerTile);
    const uint16_t height_tiles =
        static_cast<uint16_t>(screen_height / kPixelsPerTile);
    const bool color_256 = (bgcnt & UINT16_C(0x0080)) != 0u;
    const uint32_t tile_bytes = color_256 ? 64u : 32u;
    const NdsDisplayControl::BgScrollState scroll =
        display.bg_scroll(engine_index, bg_index);
    const NdsDisplayControl::WindowBlendState effects =
        display.window_blend(engine_index);
    const bool mosaic_enabled = (bgcnt & UINT16_C(0x0040)) != 0u;

    for (uint16_t x = 0u; x < 256u; ++x) {
        const Nds2dMosaicSample sample = Nds2dEffects::mosaic_sample(
            x, scanline, effects.mosaic, false, mosaic_enabled);

        const uint16_t source_x = static_cast<uint16_t>(
            (sample.x + scroll.horizontal) & (screen_width - 1u));
        const uint16_t source_y = static_cast<uint16_t>(
            (sample.y + scroll.vertical) & (screen_height - 1u));
        const uint16_t tile_x =
            static_cast<uint16_t>(source_x / kPixelsPerTile);
        const uint16_t tile_y =
            static_cast<uint16_t>(source_y / kPixelsPerTile);
        const uint16_t pixel_x =
            static_cast<uint16_t>(source_x & (kPixelsPerTile - 1u));
        const uint16_t pixel_y =
            static_cast<uint16_t>(source_y & (kPixelsPerTile - 1u));

        const uint16_t screen_blocks_across =
            static_cast<uint16_t>(width_tiles / kScreenBlockTiles);
        const uint64_t screen_block_index =
            static_cast<uint64_t>(tile_y / kScreenBlockTiles) *
                screen_blocks_across +
            tile_x / kScreenBlockTiles;
        const uint64_t map_entry_index =
            static_cast<uint64_t>(tile_y % kScreenBlockTiles) *
                kScreenBlockTiles +
            tile_x % kScreenBlockTiles;
        const uint64_t map_offset =
            screen_base + screen_block_index * kScreenBlockBytes +
            map_entry_index * sizeof(uint16_t);

        uint32_t map_address = 0u;
        if (!add_guest_offset(bg_vram_base, map_offset, map_address)) {
            return memory_error(Status::AddressOverflow);
        }
        const ReadResult map_entry =
            memory_.read(NDS_ARM9, map_address, 2u);
        if (!map_entry) return memory_error(map_entry.status);

        const uint32_t tile_index =
            map_entry.value & UINT32_C(0x03FF);
        const bool horizontal_flip =
            (map_entry.value & (UINT32_C(1) << 10u)) != 0u;
        const bool vertical_flip =
            (map_entry.value & (UINT32_C(1) << 11u)) != 0u;
        const uint32_t palette_bank =
            (map_entry.value >> 12u) & UINT32_C(0x000F);
        const uint32_t tile_pixel_x =
            horizontal_flip ? 7u - pixel_x : pixel_x;
        const uint32_t tile_pixel_y =
            vertical_flip ? 7u - pixel_y : pixel_y;
        const uint32_t pixel_index =
            tile_pixel_y * 8u + tile_pixel_x;

        const uint64_t tile_base =
            character_base + static_cast<uint64_t>(tile_index) * tile_bytes;
        uint32_t pixel_address = 0u;
        uint32_t color_index = 0u;
        if (color_256) {
            if (!add_guest_offset(
                    bg_vram_base, tile_base + pixel_index, pixel_address)) {
                return memory_error(Status::AddressOverflow);
            }
            const ReadResult pixel =
                memory_.read(NDS_ARM9, pixel_address, 1u);
            if (!pixel) return memory_error(pixel.status);
            color_index = pixel.value & UINT32_C(0xFF);
        } else {
            const uint32_t packed_byte = pixel_index / 2u;
            if (!add_guest_offset(
                    bg_vram_base, tile_base + packed_byte, pixel_address)) {
                return memory_error(Status::AddressOverflow);
            }
            const ReadResult pixel =
                memory_.read(NDS_ARM9, pixel_address, 1u);
            if (!pixel) return memory_error(pixel.status);
            color_index =
                (pixel.value >> ((pixel_index & 1u) * 4u)) & UINT32_C(0x0F);
        }

        Nds2dBgPixel &output_pixel = next_line[x];
        if (color_index == 0u) {
            output_pixel.transparent = true;
            continue;
        }

        const uint32_t palette_index = color_256
            ? color_index
            : palette_bank * 16u + color_index;
        const uint32_t palette_address =
            bg_palette_base + palette_index * sizeof(uint16_t);
        const ReadResult palette =
            memory_.read(NDS_ARM9, palette_address, 2u);
        if (!palette) return memory_error(palette.status);

        output_pixel.rgb555 =
            static_cast<uint16_t>(palette.value & UINT32_C(0x7FFF));
        output_pixel.transparent = false;
    }

    output = next_line;
    return {Nds2dBgFetchStatus::Ok, Status::Ok};
}

} // namespace oracis::memory
