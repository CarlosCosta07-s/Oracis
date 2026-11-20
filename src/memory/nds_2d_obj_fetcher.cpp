#include "oracis/memory/nds_2d_obj_fetcher.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

#include "oracis/memory/nds_2d_effects.hpp"

namespace oracis::memory {
namespace {

constexpr uint32_t kEngineAOamBase = UINT32_C(0x07000000);
constexpr uint32_t kEngineBOamBase = UINT32_C(0x07000400);
constexpr uint32_t kEngineAObjVramBase = UINT32_C(0x06400000);
constexpr uint32_t kEngineBObjVramBase = UINT32_C(0x06600000);
constexpr uint32_t kEngineAObjPaletteBase = UINT32_C(0x05000200);
constexpr uint32_t kEngineBObjPaletteBase = UINT32_C(0x05000600);
constexpr uint32_t kObjEnable = UINT32_C(1) << 12u;
constexpr uint32_t kObjWindowEnable = UINT32_C(1) << 15u;
constexpr uint32_t kObjOneDimensional = UINT32_C(1) << 4u;
constexpr uint32_t kObjBoundaryShift = 20u;
constexpr uint32_t kObjCount = 128u;
constexpr uint32_t kObjEntryBytes = 8u;
constexpr uint32_t kObj2dCharacterBytes = UINT32_C(0x8000);

constexpr std::array<std::array<uint8_t, 4>, 3> kObjWidths{{
    {{8u, 16u, 32u, 64u}},
    {{16u, 32u, 32u, 64u}},
    {{8u, 8u, 16u, 32u}}
}};

constexpr std::array<std::array<uint8_t, 4>, 3> kObjHeights{{
    {{8u, 16u, 32u, 64u}},
    {{8u, 8u, 16u, 32u}},
    {{16u, 32u, 32u, 64u}}
}};

Nds2dObjFetchResult memory_error(
    uint8_t object_index, Status status) noexcept
{
    return {Nds2dObjFetchStatus::GuestMemoryError, status, object_index};
}

Nds2dObjFetchResult unsupported(
    Nds2dObjFetchStatus status, uint8_t object_index) noexcept
{
    return {status, Status::Ok, object_index};
}

bool read_halfword(
    const GuestMemory &memory,
    uint32_t address,
    uint16_t &value,
    Status &status) noexcept
{
    const ReadResult result = memory.read(NDS_ARM9, address, 2u);
    status = result.status;
    if (!result) return false;
    value = static_cast<uint16_t>(result.value);
    return true;
}

bool read_byte(
    const GuestMemory &memory,
    uint32_t address,
    uint8_t &value,
    Status &status) noexcept
{
    const ReadResult result = memory.read(NDS_ARM9, address, 1u);
    status = result.status;
    if (!result) return false;
    value = static_cast<uint8_t>(result.value);
    return true;
}

bool add_address(
    uint32_t base, uint64_t offset, uint32_t &address) noexcept
{
    const uint64_t result = static_cast<uint64_t>(base) + offset;
    if (result > UINT32_MAX) return false;
    address = static_cast<uint32_t>(result);
    return true;
}

bool outranks(
    uint8_t priority,
    uint8_t object_index,
    const Nds2dObjPixel &current) noexcept
{
    return current.transparent ||
        priority < current.priority ||
        (priority == current.priority &&
         object_index < current.object_index);
}

} // namespace

Nds2dObjFetchResult Nds2dObjFetcher::render_scanline(
    unsigned engine_index,
    const NdsDisplayControl &display,
    uint16_t scanline,
    Nds2dObjScanline &output) const noexcept
{
    if (engine_index > 1u) {
        return {Nds2dObjFetchStatus::InvalidEngine, Status::InvalidCpu};
    }
    if (scanline >= NdsDisplayControl::VisibleScanlines) {
        return {Nds2dObjFetchStatus::InvalidScanline, Status::InvalidRegion};
    }
    if (!display.installed()) {
        return {Nds2dObjFetchStatus::GuestMemoryError, Status::Unmapped};
    }

    const uint32_t dispcnt = display.dispcnt(engine_index);
    const bool obj_enabled = (dispcnt & kObjEnable) != 0u;
    const bool obj_window_enabled = (dispcnt & kObjWindowEnable) != 0u;
    if (!obj_enabled && !obj_window_enabled) {
        output = {};
        return {Nds2dObjFetchStatus::Disabled, Status::Ok};
    }

    const uint32_t oam_base =
        engine_index == 0u ? kEngineAOamBase : kEngineBOamBase;
    const uint32_t obj_vram_base =
        engine_index == 0u ? kEngineAObjVramBase : kEngineBObjVramBase;
    const uint32_t obj_palette_base =
        engine_index == 0u ? kEngineAObjPaletteBase : kEngineBObjPaletteBase;
    const NdsDisplayControl::WindowBlendState effects =
        display.window_blend(engine_index);
    const bool one_dimensional = (dispcnt & kObjOneDimensional) != 0u;
    const uint32_t boundary = UINT32_C(32) <<
        ((dispcnt >> kObjBoundaryShift) & UINT32_C(3));

    if (engine_index == 1u && one_dimensional && boundary > UINT32_C(128)) {
        return unsupported(
            Nds2dObjFetchStatus::UnsupportedMapping, UINT8_C(0xFF));
    }

    Nds2dObjScanline next_line{};

    for (uint32_t object = 0u; object < kObjCount; ++object) {
        const uint8_t object_index = static_cast<uint8_t>(object);
        const uint32_t entry_address =
            oam_base + object * kObjEntryBytes;
        uint16_t attr0 = 0u;
        uint16_t attr1 = 0u;
        uint16_t attr2 = 0u;
        Status memory_status = Status::Ok;

        if (!read_halfword(memory_, entry_address, attr0, memory_status) ||
            !read_halfword(memory_, entry_address + 2u, attr1, memory_status) ||
            !read_halfword(memory_, entry_address + 4u, attr2, memory_status)) {
            return memory_error(object_index, memory_status);
        }

        const bool affine = (attr0 & UINT16_C(0x0100)) != 0u;
        const bool double_size = affine &&
            (attr0 & UINT16_C(0x0200)) != 0u;
        if (!affine && (attr0 & UINT16_C(0x0200)) != 0u) {
            /* Non-affine Attr0 bit 9 disables this OBJ. */
            continue;
        }

        const unsigned obj_mode =
            static_cast<unsigned>((attr0 >> 10u) & UINT16_C(3));
        if ((obj_mode == 0u || obj_mode == 1u) && !obj_enabled) continue;
        if (obj_mode == 2u && !obj_window_enabled) continue;
        if (obj_mode == 3u && !obj_enabled) continue;

        const unsigned shape =
            static_cast<unsigned>((attr0 >> 14u) & UINT16_C(3));
        const unsigned size =
            static_cast<unsigned>((attr1 >> 14u) & UINT16_C(3));
        if (shape == 3u) {
            return unsupported(
                Nds2dObjFetchStatus::UnsupportedShape, object_index);
        }

        const uint32_t width = kObjWidths[shape][size];
        const uint32_t height = kObjHeights[shape][size];
        const uint32_t bounds_width = double_size ? width * 2u : width;
        const uint32_t bounds_height = double_size ? height * 2u : height;
        const uint32_t y = attr0 & UINT16_C(0x00FF);
        const uint32_t source_y =
            (static_cast<uint32_t>(scanline) + UINT32_C(256) - y) &
            UINT32_C(0xFF);
        if (source_y >= bounds_height) continue;

        const int raw_x = static_cast<int>(attr1 & UINT16_C(0x01FF));
        const int sprite_x = raw_x >= 256 ? raw_x - 512 : raw_x;
        const int right = sprite_x + static_cast<int>(bounds_width);
        if (right <= 0 || sprite_x >= 256) continue;

        if (affine) {
            return unsupported(
                Nds2dObjFetchStatus::UnsupportedAffine, object_index);
        }
        if (obj_mode == 3u) {
            return unsupported(
                Nds2dObjFetchStatus::UnsupportedBitmap, object_index);
        }

        const bool color_256 =
            (attr0 & UINT16_C(0x2000)) != 0u;
        const bool horizontal_flip =
            (attr1 & UINT16_C(0x1000)) != 0u;
        const bool vertical_flip =
            (attr1 & UINT16_C(0x2000)) != 0u;
        const bool mosaic_enabled =
            (attr0 & UINT16_C(0x1000)) != 0u;
        const uint32_t palette_bank =
            static_cast<uint32_t>((attr2 >> 12u) & UINT16_C(0x000F));
        const uint8_t priority =
            static_cast<uint8_t>((attr2 >> 10u) & UINT16_C(3));
        uint32_t tile_index =
            static_cast<uint32_t>(attr2 & UINT16_C(0x03FF));
        const uint32_t tile_bytes = color_256 ? 64u : 32u;
        const uint32_t tiles_wide = width / 8u;

        /* 8bpp tiles use two consecutive 32-byte character indices. */
        if (color_256) tile_index &= ~UINT32_C(1);

        const uint32_t screen_left =
            static_cast<uint32_t>(std::max(0, sprite_x));
        const uint32_t screen_right =
            static_cast<uint32_t>(std::min(256, right));

        for (uint32_t screen_x = screen_left;
             screen_x < screen_right;
             ++screen_x) {
            const uint32_t local_x = static_cast<uint32_t>(
                static_cast<int>(screen_x) - sprite_x);
            const Nds2dMosaicSample mosaic = Nds2dEffects::mosaic_sample(
                static_cast<uint16_t>(local_x),
                static_cast<uint16_t>(source_y),
                effects.mosaic,
                true,
                mosaic_enabled);
            const uint32_t sampled_x = mosaic.x;
            const uint32_t sampled_y = mosaic.y;
            const uint32_t source_x = horizontal_flip
                ? width - 1u - sampled_x
                : sampled_x;
            const uint32_t source_row = vertical_flip
                ? height - 1u - sampled_y
                : sampled_y;
            const uint32_t tile_x = source_x / 8u;
            const uint32_t tile_y = source_row / 8u;
            const uint32_t pixel_x = source_x & 7u;
            const uint32_t pixel_y = source_row & 7u;
            const uint32_t pixel_in_tile = pixel_y * 8u + pixel_x;

            uint64_t tile_offset = 0u;
            if (one_dimensional) {
                const uint64_t linear_tile =
                    static_cast<uint64_t>(tile_y) * tiles_wide + tile_x;
                tile_offset =
                    static_cast<uint64_t>(tile_index) * boundary +
                    linear_tile * tile_bytes;
            } else {
                tile_offset =
                    static_cast<uint64_t>(tile_index) * 32u +
                    static_cast<uint64_t>(tile_y) * UINT32_C(0x400) +
                    static_cast<uint64_t>(tile_x) * tile_bytes;
            }

            const uint32_t engine_obj_bytes =
                engine_index == 0u ? UINT32_C(0x40000) : UINT32_C(0x20000);
            const uint64_t pixel_offset = color_256
                ? pixel_in_tile
                : pixel_in_tile / 2u;
            const uint64_t final_offset = tile_offset + pixel_offset;
            if ((!one_dimensional &&
                 final_offset >= kObj2dCharacterBytes) ||
                (one_dimensional && final_offset >= engine_obj_bytes)) {
                return memory_error(object_index, Status::Unmapped);
            }

            uint32_t pixel_address = 0u;
            if (!add_address(obj_vram_base, final_offset, pixel_address)) {
                return memory_error(object_index, Status::AddressOverflow);
            }

            uint8_t color_index = 0u;
            if (!read_byte(
                    memory_, pixel_address, color_index, memory_status)) {
                return memory_error(object_index, memory_status);
            }
            if (!color_256) {
                color_index = static_cast<uint8_t>(
                    (color_index >> ((pixel_in_tile & 1u) * 4u)) &
                    UINT8_C(0x0F));
            }
            if (color_index == 0u) continue;

            if (obj_mode == 2u) {
                next_line.object_window_coverage[screen_x] = true;
                continue;
            }

            const uint32_t palette_index = color_256
                ? color_index
                : palette_bank * 16u + color_index;
            const uint32_t palette_address =
                obj_palette_base + palette_index * 2u;
            uint16_t palette_color = 0u;
            if (!read_halfword(
                    memory_, palette_address, palette_color, memory_status)) {
                return memory_error(object_index, memory_status);
            }

            Nds2dObjPixel &current = next_line.pixels[screen_x];
            if (outranks(priority, object_index, current)) {
                current.rgb555 =
                    static_cast<uint16_t>(palette_color & UINT16_C(0x7FFF));
                current.priority = priority;
                current.object_index = object_index;
                current.transparent = false;
                current.semi_transparent = obj_mode == 1u;
            }
        }
    }

    output = next_line;
    return {};
}

} // namespace oracis::memory
