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
constexpr uint32_t kObjBitmapWidth256 = UINT32_C(1) << 5u;
constexpr uint32_t kObjBitmapOneDimensional = UINT32_C(1) << 6u;
constexpr uint32_t kObjBitmapVramExtended = UINT32_C(1) << 22u;
constexpr uint32_t kObjBitmapColorDepth = UINT32_C(1) << 13u;
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

struct AffineMatrix {
    int32_t pa = 0;
    int32_t pb = 0;
    int32_t pc = 0;
    int32_t pd = 0;
};

int32_t signed_halfword(uint16_t value) noexcept
{
    return value < UINT16_C(0x8000)
        ? static_cast<int32_t>(value)
        : static_cast<int32_t>(value) - INT32_C(0x10000);
}

/* Signed 8.8 fixed-point conversion, rounded toward negative infinity. */
int32_t floor_fixed_8_8(int64_t value) noexcept
{
    if (value >= 0) {
        return static_cast<int32_t>(value / INT64_C(256));
    }
    return static_cast<int32_t>(
        -(((-value) + INT64_C(255)) / INT64_C(256)));
}

bool read_affine_matrix(
    const GuestMemory &memory,
    uint32_t oam_base,
    uint32_t matrix_index,
    AffineMatrix &matrix,
    Status &status) noexcept
{
    const uint32_t matrix_base =
        oam_base + matrix_index * UINT32_C(32);
    uint16_t raw_pa = 0u;
    uint16_t raw_pb = 0u;
    uint16_t raw_pc = 0u;
    uint16_t raw_pd = 0u;

    if (!read_halfword(memory, matrix_base + 6u, raw_pa, status) ||
        !read_halfword(memory, matrix_base + 14u, raw_pb, status) ||
        !read_halfword(memory, matrix_base + 22u, raw_pc, status) ||
        !read_halfword(memory, matrix_base + 30u, raw_pd, status)) {
        return false;
    }

    matrix = {
        signed_halfword(raw_pa),
        signed_halfword(raw_pb),
        signed_halfword(raw_pc),
        signed_halfword(raw_pd)
    };
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
        const bool bitmap_obj = obj_mode == 3u;
        if ((obj_mode == 0u || obj_mode == 1u) && !obj_enabled) continue;
        if (obj_mode == 2u && !obj_window_enabled) continue;
        if (bitmap_obj && !obj_enabled) continue;

        /*
         * Bitmap OBJ stores alphaOAM in Attr2[12:15]. A zero coefficient
         * makes the whole OBJ transparent and must not reach the rasterizer.
         */
        const uint8_t bitmap_alpha_oam = static_cast<uint8_t>(
            (attr2 >> 12u) & UINT16_C(0x000F));
        if (bitmap_obj && bitmap_alpha_oam == 0u) continue;

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
        const uint32_t destination_y =
            (static_cast<uint32_t>(scanline) + UINT32_C(256) - y) &
            UINT32_C(0xFF);
        if (destination_y >= bounds_height) continue;

        const int raw_x = static_cast<int>(attr1 & UINT16_C(0x01FF));
        const int sprite_x = raw_x >= 256 ? raw_x - 512 : raw_x;
        const int right = sprite_x + static_cast<int>(bounds_width);
        if (right <= 0 || sprite_x >= 256) continue;

        const unsigned bitmap_mapping = static_cast<unsigned>(
            (dispcnt >> 5u) & UINT32_C(3));
        const bool bitmap_one_dimensional =
            bitmap_obj && bitmap_mapping == 2u;
        if (bitmap_obj &&
            (((attr0 & kObjBitmapColorDepth) != 0u) ||
             bitmap_mapping == 3u)) {
            return unsupported(
                Nds2dObjFetchStatus::UnsupportedMapping, object_index);
        }

        const uint32_t bitmap_width =
            (dispcnt & kObjBitmapWidth256) != 0u
                ? UINT32_C(256) : UINT32_C(128);
        const uint32_t bitmap_boundary =
            engine_index == 0u &&
            (dispcnt & kObjBitmapVramExtended) != 0u
                ? UINT32_C(256) : UINT32_C(128);
        const uint32_t bitmap_vram_capacity = bitmap_obj
            ? (bitmap_one_dimensional
                ? (engine_index == 0u &&
                   (dispcnt & kObjBitmapVramExtended) != 0u
                    ? UINT32_C(0x40000) : UINT32_C(0x20000))
                : kObj2dCharacterBytes)
            : 0u;

        AffineMatrix matrix{};
        if (affine) {
            const uint32_t matrix_index =
                static_cast<uint32_t>((attr1 >> 9u) & UINT16_C(0x001F));
            if (!read_affine_matrix(
                    memory_, oam_base, matrix_index, matrix, memory_status)) {
                return memory_error(object_index, memory_status);
            }
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
                static_cast<uint16_t>(destination_y),
                effects.mosaic,
                true,
                mosaic_enabled);
            const uint32_t sampled_x = mosaic.x;
            const uint32_t sampled_y = mosaic.y;

            int32_t source_x = 0;
            int32_t source_row = 0;
            if (affine) {
                const int32_t destination_dx =
                    static_cast<int32_t>(sampled_x) -
                    static_cast<int32_t>(bounds_width / 2u);
                const int32_t destination_dy =
                    static_cast<int32_t>(sampled_y) -
                    static_cast<int32_t>(bounds_height / 2u);
                source_x = floor_fixed_8_8(
                    static_cast<int64_t>(matrix.pa) * destination_dx +
                    static_cast<int64_t>(matrix.pb) * destination_dy) +
                    static_cast<int32_t>(width / 2u);
                source_row = floor_fixed_8_8(
                    static_cast<int64_t>(matrix.pc) * destination_dx +
                    static_cast<int64_t>(matrix.pd) * destination_dy) +
                    static_cast<int32_t>(height / 2u);
            } else {
                source_x = static_cast<int32_t>(
                    horizontal_flip ? width - 1u - sampled_x : sampled_x);
                source_row = static_cast<int32_t>(
                    vertical_flip ? height - 1u - sampled_y : sampled_y);
            }

            /* Transformed texels outside the source OBJ are transparent. */
            if (source_x < 0 || source_row < 0 ||
                source_x >= static_cast<int32_t>(width) ||
                source_row >= static_cast<int32_t>(height)) {
                continue;
            }

            if (bitmap_obj) {
                const uint32_t bitmap_source_x =
                    static_cast<uint32_t>(source_x);
                const uint32_t bitmap_source_y =
                    static_cast<uint32_t>(source_row);
                uint64_t bitmap_pixel_offset = 0u;

                if (bitmap_one_dimensional) {
                    /*
                     * 1D bitmap data is a linear, tightly packed RGB555
                     * surface starting at TileName * 128/256 bytes.
                     * Its source width is the OAM object's width.
                     */
                    const uint64_t bitmap_base =
                        static_cast<uint64_t>(tile_index) * bitmap_boundary;
                    const uint64_t linear_pixel =
                        static_cast<uint64_t>(bitmap_source_y) * width +
                        bitmap_source_x;
                    bitmap_pixel_offset =
                        bitmap_base + linear_pixel * sizeof(uint16_t);
                } else {
                    /*
                     * 2D bitmap data is tiled in 8x8 character-name units,
                     * but pixels inside each tile remain scanline-linear.
                     * Name bits are split into X (low bits) and Y (high).
                     */
                    const uint32_t tiles_wide = bitmap_width / 8u;
                    const uint32_t tile_x_mask = tiles_wide - 1u;
                    const uint64_t bitmap_tile_name =
                        static_cast<uint64_t>(tile_index) +
                        static_cast<uint64_t>(bitmap_source_y / 8u) *
                            tiles_wide +
                        bitmap_source_x / 8u;
                    const uint64_t tile_base =
                        (bitmap_tile_name & tile_x_mask) *
                            UINT64_C(0x10) +
                        (bitmap_tile_name & ~static_cast<uint64_t>(
                            tile_x_mask)) * UINT64_C(0x80);
                    const uint64_t in_tile_pixel =
                        static_cast<uint64_t>(bitmap_source_y & 7u) *
                            bitmap_width * sizeof(uint16_t) +
                        static_cast<uint64_t>(bitmap_source_x & 7u) *
                            sizeof(uint16_t);
                    bitmap_pixel_offset = tile_base + in_tile_pixel;
                }

                if (bitmap_pixel_offset + sizeof(uint16_t) >
                    bitmap_vram_capacity) {
                    return memory_error(object_index, Status::Unmapped);
                }

                uint32_t bitmap_pixel_address = 0u;
                if (!add_address(
                        obj_vram_base, bitmap_pixel_offset,
                        bitmap_pixel_address)) {
                    return memory_error(
                        object_index, Status::AddressOverflow);
                }

                uint16_t bitmap_pixel = 0u;
                if (!read_halfword(
                        memory_, bitmap_pixel_address,
                        bitmap_pixel, memory_status)) {
                    return memory_error(object_index, memory_status);
                }

                /* Bit 15 is alphaBMP; a clear flag makes this texel transparent. */
                if ((bitmap_pixel & UINT16_C(0x8000)) == 0u) continue;

                const uint8_t bitmap_alpha_eva = static_cast<uint8_t>(
                    bitmap_alpha_oam + 1u);
                const Nds2dObjPixel candidate{
                    static_cast<uint16_t>(
                        bitmap_pixel & UINT16_C(0x7FFF)),
                    priority,
                    object_index,
                    false,
                    false,
                    bitmap_alpha_eva
                };
                Nds2dObjPixel &top = next_line.pixels[screen_x];
                Nds2dObjPixel &second = next_line.second_pixels[screen_x];

                if (outranks(priority, object_index, top)) {
                    second = top;
                    top = candidate;
                } else if (outranks(priority, object_index, second)) {
                    second = candidate;
                }
                continue;
            }

            const uint32_t tile_x =
                static_cast<uint32_t>(source_x) / 8u;
            const uint32_t tile_y =
                static_cast<uint32_t>(source_row) / 8u;
            const uint32_t pixel_x = static_cast<uint32_t>(source_x) & 7u;
            const uint32_t pixel_y = static_cast<uint32_t>(source_row) & 7u;
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

            const Nds2dObjPixel candidate{
                static_cast<uint16_t>(palette_color & UINT16_C(0x7FFF)),
                priority,
                object_index,
                false,
                obj_mode == 1u
            };
            Nds2dObjPixel &top = next_line.pixels[screen_x];
            Nds2dObjPixel &second = next_line.second_pixels[screen_x];

            if (outranks(priority, object_index, top)) {
                second = top;
                top = candidate;
            } else if (outranks(priority, object_index, second)) {
                second = candidate;
            }
        }
    }

    output = next_line;
    return {};
}

} // namespace oracis::memory
