#include "oracis/memory/nds_2d_affine_bg_fetcher.hpp"

#include <cstdint>

#include "oracis/memory/nds_2d_effects.hpp"

namespace oracis::memory {
namespace {

constexpr uint32_t kEngineABgVramBase = UINT32_C(0x06000000);
constexpr uint32_t kEngineBBgVramBase = UINT32_C(0x06200000);
constexpr uint32_t kEngineABgPaletteBase = UINT32_C(0x05000000);
constexpr uint32_t kEngineBBgPaletteBase = UINT32_C(0x05000400);
constexpr uint32_t kBgEnableShift = 8u;
constexpr uint32_t kExtendedBgPaletteEnable = UINT32_C(1) << 30u;
constexpr uint32_t kScreenBlockBytes = UINT32_C(0x0800);
constexpr uint32_t kAffineTilePixels = 8u;
constexpr uint32_t kAffineTileBytes = 64u;

Nds2dBgFetchResult memory_error(Status status) noexcept
{
    return {Nds2dBgFetchStatus::GuestMemoryError, status};
}

int64_t floor_fixed_8_8(int64_t value) noexcept
{
    if (value >= 0) return value / INT64_C(256);
    return -(((-value) + INT64_C(255)) / INT64_C(256));
}

int64_t positive_modulo(int64_t value, int64_t modulus) noexcept
{
    const int64_t remainder = value % modulus;
    return remainder < 0 ? remainder + modulus : remainder;
}

} // namespace

bool Nds2dAffineBgFetcher::add_guest_offset(
    uint32_t base, uint64_t offset, uint32_t &address) noexcept
{
    const uint64_t result = static_cast<uint64_t>(base) + offset;
    if (result > UINT32_MAX) return false;
    address = static_cast<uint32_t>(result);
    return true;
}

bool Nds2dAffineBgFetcher::is_affine_tile_bg(
    unsigned engine_index,
    unsigned bg_index,
    uint32_t dispcnt) noexcept
{
    if (engine_index > 1u || bg_index < 2u || bg_index > 3u) {
        return false;
    }

    const unsigned mode = static_cast<unsigned>(dispcnt & UINT32_C(7));
    if (mode == 1u) return bg_index == 3u;
    if (mode == 2u) return true;
    if (mode == 4u) return bg_index == 2u;
    return false;
}

bool Nds2dAffineBgFetcher::is_extended_bg_layer(
    unsigned engine_index,
    unsigned bg_index,
    uint32_t dispcnt) noexcept
{
    if (engine_index > 1u || bg_index < 2u || bg_index > 3u) {
        return false;
    }

    const unsigned mode = static_cast<unsigned>(dispcnt & UINT32_C(7));
    if (mode == 3u || mode == 4u) return bg_index == 3u;
    if (mode == 5u) return true;
    return false;
}

Nds2dBgFetchResult Nds2dAffineBgFetcher::render_scanline(
    unsigned engine_index,
    unsigned bg_index,
    const NdsDisplayControl &display,
    uint16_t scanline,
    std::array<Nds2dBgPixel, 256> &output) const noexcept
{
    if (engine_index > 1u) {
        return {Nds2dBgFetchStatus::InvalidEngine, Status::InvalidRegion};
    }
    if (bg_index >= NdsDisplayControl::BgCount) {
        return {Nds2dBgFetchStatus::InvalidBackground, Status::InvalidRegion};
    }
    if (scanline >= NdsDisplayControl::VisibleScanlines) {
        return {Nds2dBgFetchStatus::InvalidScanline, Status::InvalidRegion};
    }
    if (!display.installed()) {
        return {Nds2dBgFetchStatus::DisplayNotInstalled, Status::Unmapped};
    }

    const uint32_t dispcnt = display.dispcnt(engine_index);
    const uint16_t bgcnt = display.bg_control(engine_index, bg_index);
    const uint32_t enable_bit =
        UINT32_C(1) << (kBgEnableShift + bg_index);

    std::array<Nds2dBgPixel, 256> next_line{};
    for (Nds2dBgPixel &pixel : next_line) {
        pixel.priority = static_cast<uint8_t>(bgcnt & UINT16_C(3));
        pixel.bg_index = static_cast<uint8_t>(bg_index);
    }

    if ((dispcnt & enable_bit) == 0u) {
        output = next_line;
        return {Nds2dBgFetchStatus::Disabled, Status::Ok};
    }
    const bool classic_affine =
        is_affine_tile_bg(engine_index, bg_index, dispcnt);
    const bool extended_bg =
        is_extended_bg_layer(engine_index, bg_index, dispcnt);
    const bool extended_bitmap =
        extended_bg && (bgcnt & UINT16_C(0x0080)) != 0u;
    const bool extended_tile_map =
        extended_bg && !extended_bitmap;
    /*
     * DISPCNT.30 selects the external palette only for 16-bit extended
     * affine tile maps. Classic affine maps and both bitmap formats keep
     * using the standard 256-color BG palette or direct RGB555 data.
     */
    if ((!classic_affine && !extended_bg) ||
        (extended_tile_map &&
         (dispcnt & kExtendedBgPaletteEnable) != 0u &&
         vram_control_ == nullptr)) {
        return {Nds2dBgFetchStatus::UnsupportedMode, Status::Ok};
    }

    const uint32_t bg_vram_base =
        engine_index == 0u ? kEngineABgVramBase : kEngineBBgVramBase;
    const uint32_t bg_palette_base =
        engine_index == 0u ? kEngineABgPaletteBase : kEngineBBgPaletteBase;

    const uint32_t character_base_block =
        static_cast<uint32_t>((bgcnt >> 2u) & UINT16_C(0x000F));
    const uint32_t screen_base_block =
        static_cast<uint32_t>((bgcnt >> 8u) & UINT16_C(0x001F));
    const uint32_t character_base_offset = engine_index == 0u
        ? ((dispcnt >> 24u) & UINT32_C(7)) * UINT32_C(0x10000)
        : 0u;
    const uint32_t screen_base_offset = engine_index == 0u
        ? ((dispcnt >> 27u) & UINT32_C(7)) * UINT32_C(0x10000)
        : 0u;
    const uint64_t character_base =
        static_cast<uint64_t>(character_base_offset) +
        static_cast<uint64_t>(character_base_block) * UINT32_C(0x4000);
    /*
     * Bitmap BGs use BGxCNT screen-base blocks in 16 KiB units and ignore
     * the DISPCNT 64 KiB screen-base extension used by tile maps.
     */
    const uint64_t screen_base = extended_bitmap
        ? static_cast<uint64_t>(screen_base_block) * UINT32_C(0x4000)
        : static_cast<uint64_t>(screen_base_offset) +
            static_cast<uint64_t>(screen_base_block) * kScreenBlockBytes;

    const unsigned size =
        static_cast<unsigned>((bgcnt >> 14u) & UINT16_C(3));
    const uint32_t tiles_wide = UINT32_C(16) << size;
    const uint32_t dimensions = tiles_wide * kAffineTilePixels;
    const uint32_t bitmap_width = size < 2u
        ? (UINT32_C(128) << size)
        : UINT32_C(512);
    const uint32_t bitmap_height = size == 0u
        ? UINT32_C(128)
        : (size == 1u ? UINT32_C(256)
                      : (size == 2u ? UINT32_C(256) : UINT32_C(512)));
    const bool direct_color_bitmap =
        extended_bitmap && (bgcnt & UINT16_C(0x0004)) != 0u;
    const uint64_t bitmap_bytes =
        static_cast<uint64_t>(bitmap_width) * bitmap_height *
        (direct_color_bitmap ? UINT64_C(2) : UINT64_C(1));
    if (extended_bitmap && engine_index == 1u &&
        bitmap_bytes > UINT64_C(0x20000)) {
        return {Nds2dBgFetchStatus::UnsupportedMode, Status::Ok};
    }
    const uint32_t source_width = extended_bitmap ? bitmap_width : dimensions;
    const uint32_t source_height = extended_bitmap ? bitmap_height : dimensions;
    const bool wraparound = (bgcnt & UINT16_C(0x2000)) != 0u;
    const bool mosaic_enabled = (bgcnt & UINT16_C(0x0040)) != 0u;
    const NdsDisplayControl::BgAffineState affine =
        display.bg_affine(engine_index, bg_index);
    const NdsDisplayControl::WindowBlendState effects =
        display.window_blend(engine_index);
    const int64_t current_line =
        static_cast<int64_t>(display.current_scanline());

    for (uint16_t x = 0u; x < 256u; ++x) {
        const Nds2dMosaicSample sample = Nds2dEffects::mosaic_sample(
            x, scanline, effects.mosaic, false, mosaic_enabled);
        const int64_t sample_line_delta =
            static_cast<int64_t>(sample.y) - current_line;

        const int64_t source_x_fixed =
            static_cast<int64_t>(affine.line_reference_x) +
            static_cast<int64_t>(affine.pb) * sample_line_delta +
            static_cast<int64_t>(affine.pa) * sample.x;
        const int64_t source_y_fixed =
            static_cast<int64_t>(affine.line_reference_y) +
            static_cast<int64_t>(affine.pd) * sample_line_delta +
            static_cast<int64_t>(affine.pc) * sample.x;

        int64_t source_x = floor_fixed_8_8(source_x_fixed);
        int64_t source_y = floor_fixed_8_8(source_y_fixed);
        if (wraparound) {
            source_x = positive_modulo(source_x, source_width);
            source_y = positive_modulo(source_y, source_height);
        } else if (source_x < 0 || source_y < 0 ||
                   source_x >= source_width || source_y >= source_height) {
            continue;
        }

        const uint32_t pixel_x = static_cast<uint32_t>(source_x);
        const uint32_t pixel_y = static_cast<uint32_t>(source_y);

        if (extended_bitmap) {
            const uint64_t bitmap_pixel_index =
                static_cast<uint64_t>(pixel_y) * bitmap_width + pixel_x;
            const uint64_t bitmap_pixel_offset =
                screen_base + bitmap_pixel_index *
                    (direct_color_bitmap ? UINT64_C(2) : UINT64_C(1));
            uint32_t bitmap_pixel_address = 0u;
            if (!add_guest_offset(
                    bg_vram_base, bitmap_pixel_offset,
                    bitmap_pixel_address)) {
                return memory_error(Status::AddressOverflow);
            }

            if (direct_color_bitmap) {
                const ReadResult color =
                    memory_.read(NDS_ARM9, bitmap_pixel_address, 2u);
                if (!color) return memory_error(color.status);
                if ((color.value & UINT32_C(0x8000)) == 0u) continue;
                next_line[x].rgb555 =
                    static_cast<uint16_t>(color.value & UINT32_C(0x7FFF));
            } else {
                const ReadResult color_index =
                    memory_.read(NDS_ARM9, bitmap_pixel_address, 1u);
                if (!color_index) return memory_error(color_index.status);
                const uint32_t palette_index =
                    static_cast<uint32_t>(color_index.value & UINT32_C(0xFF));
                if (palette_index == 0u) continue;

                const uint32_t palette_address =
                    bg_palette_base + palette_index * sizeof(uint16_t);
                const ReadResult color =
                    memory_.read(NDS_ARM9, palette_address, 2u);
                if (!color) return memory_error(color.status);
                next_line[x].rgb555 =
                    static_cast<uint16_t>(color.value & UINT32_C(0x7FFF));
            }
            next_line[x].transparent = false;
            continue;
        }

        const uint32_t tile_x = pixel_x / kAffineTilePixels;
        const uint32_t tile_y = pixel_y / kAffineTilePixels;
        const uint32_t map_index = tile_y * tiles_wide + tile_x;

        const uint32_t map_entry_bytes = extended_bg ? 2u : 1u;
        uint32_t map_address = 0u;
        if (!add_guest_offset(
                bg_vram_base,
                screen_base + static_cast<uint64_t>(map_index) *
                    map_entry_bytes,
                map_address)) {
            return memory_error(Status::AddressOverflow);
        }
        const ReadResult map_entry =
            memory_.read(NDS_ARM9, map_address, map_entry_bytes);
        if (!map_entry) return memory_error(map_entry.status);

        const uint32_t map_attributes =
            static_cast<uint32_t>(map_entry.value);
        const uint32_t tile_index = extended_bg
            ? map_attributes & UINT32_C(0x03FF)
            : map_attributes & UINT32_C(0x00FF);
        const uint32_t local_x = pixel_x & UINT32_C(7);
        const uint32_t local_y = pixel_y & UINT32_C(7);
        const uint32_t tile_pixel_x =
            extended_bg &&
                (map_attributes & (UINT32_C(1) << 10u)) != 0u
            ? UINT32_C(7) - local_x
            : local_x;
        const uint32_t tile_pixel_y =
            extended_bg &&
                (map_attributes & (UINT32_C(1) << 11u)) != 0u
            ? UINT32_C(7) - local_y
            : local_y;
        const uint32_t tile_pixel =
            tile_pixel_y * kAffineTilePixels + tile_pixel_x;
        const uint64_t pixel_offset =
            character_base +
            static_cast<uint64_t>(tile_index) * kAffineTileBytes +
            tile_pixel;

        uint32_t pixel_address = 0u;
        if (!add_guest_offset(bg_vram_base, pixel_offset, pixel_address)) {
            return memory_error(Status::AddressOverflow);
        }
        const ReadResult color_index =
            memory_.read(NDS_ARM9, pixel_address, 1u);
        if (!color_index) return memory_error(color_index.status);

        const uint32_t palette_index =
            static_cast<uint32_t>(color_index.value & UINT32_C(0xFF));
        if (palette_index == 0u) continue;

        if (extended_tile_map &&
            (dispcnt & kExtendedBgPaletteEnable) != 0u) {
            const uint8_t palette_number = static_cast<uint8_t>(
                (map_attributes >> 12u) & UINT32_C(0x0F));
            const ReadResult palette = vram_control_->read_extended_bg_palette(
                engine_index, bg_index, palette_number,
                static_cast<uint8_t>(palette_index));
            if (palette.status == Status::Unmapped ||
                palette.status == Status::Overlap) {
                return {Nds2dBgFetchStatus::UnsupportedMode, Status::Ok};
            }
            if (!palette) return memory_error(palette.status);
            next_line[x].rgb555 =
                static_cast<uint16_t>(palette.value & UINT32_C(0x7FFF));
        } else {
            const uint32_t palette_address =
                bg_palette_base + palette_index * sizeof(uint16_t);
            const ReadResult palette =
                memory_.read(NDS_ARM9, palette_address, 2u);
            if (!palette) return memory_error(palette.status);
            next_line[x].rgb555 =
                static_cast<uint16_t>(palette.value & UINT32_C(0x7FFF));
        }
        next_line[x].transparent = false;
    }

    output = next_line;
    return {Nds2dBgFetchStatus::Ok, Status::Ok};
}

} // namespace oracis::memory
