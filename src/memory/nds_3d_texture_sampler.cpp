#include "oracis/memory/nds_3d_texture_sampler.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace oracis::memory {
namespace {

Nds3dTextureSample failure(Nds3dTextureSampleStatus status) noexcept
{
    Nds3dTextureSample result{};
    result.status = status;
    return result;
}

Nds3dTextureSampleStatus map_vram_status(Status status) noexcept
{
    if (status == Status::Overlap) {
        return Nds3dTextureSampleStatus::AmbiguousVram;
    }
    if (status == Status::Unmapped ||
        status == Status::CpuAccessDenied ||
        status == Status::PermissionDenied) {
        return Nds3dTextureSampleStatus::UnmappedVram;
    }
    return Nds3dTextureSampleStatus::InvalidParameters;
}

} // namespace

int32_t Nds3dTextureSampler::floor_divide(
    int32_t value, int32_t divisor) noexcept
{
    const int32_t quotient = value / divisor;
    const int32_t remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

uint32_t Nds3dTextureSampler::resolve_axis(
    int16_t coordinate, uint32_t size,
    bool repeat, bool flip) noexcept
{
    const int32_t texel = floor_divide(
        static_cast<int32_t>(coordinate), 16);
    const int32_t signed_size = static_cast<int32_t>(size);

    if (!repeat) {
        return static_cast<uint32_t>(std::clamp(
            texel, 0, signed_size - 1));
    }

    const int32_t period = floor_divide(texel, signed_size);
    int32_t local = texel - (period * signed_size);
    if (flip && (period % 2) != 0) {
        local = signed_size - 1 - local;
    }
    return static_cast<uint32_t>(local);
}

ReadResult Nds3dTextureSampler::read_texture_byte(
    uint32_t byte_address) const noexcept
{
    constexpr uint32_t kTextureAddressSpace =
        NdsVramControl::BankSize * 4u;
    if (byte_address >= kTextureAddressSpace) {
        return {Status::InvalidRegion, 0u};
    }
    const uint8_t slot = static_cast<uint8_t>(
        byte_address / NdsVramControl::BankSize);
    const uint32_t offset =
        byte_address % NdsVramControl::BankSize;
    return vram_.read_texture_slot_byte(slot, offset);
}

ReadResult Nds3dTextureSampler::read_texture_halfword(
    uint32_t byte_address) const noexcept
{
    const ReadResult low = read_texture_byte(byte_address);
    if (low.status != Status::Ok) return low;
    if (byte_address == UINT32_MAX) {
        return {Status::AddressOverflow, 0u};
    }
    const ReadResult high = read_texture_byte(byte_address + 1u);
    if (high.status != Status::Ok) return high;
    return {
        Status::Ok,
        (low.value & UINT32_C(0xFF)) |
            ((high.value & UINT32_C(0xFF)) << 8u)
    };
}

Nds3dTextureSample Nds3dTextureSampler::read_palette_color(
    uint32_t byte_address, uint8_t alpha) const noexcept
{
    const ReadResult color =
        vram_.read_texture_palette_halfword(byte_address);
    if (color.status != Status::Ok) {
        return failure(map_vram_status(color.status));
    }

    Nds3dTextureSample result{};
    result.status = Nds3dTextureSampleStatus::Ok;
    result.color_rgb555 = static_cast<uint16_t>(
        color.value & UINT32_C(0x7FFF));
    result.alpha = alpha;
    result.has_texture = true;
    result.transparent = alpha == 0u;
    return result;
}

Nds3dTextureSample Nds3dTextureSampler::sample(
    uint32_t texture_parameters,
    uint32_t palette_base,
    int16_t texture_s,
    int16_t texture_t) const noexcept
{
    if (!vram_.installed()) {
        return failure(Nds3dTextureSampleStatus::UnmappedVram);
    }

    const uint8_t format = static_cast<uint8_t>(
        (texture_parameters >> 26u) & UINT32_C(7));
    if (format == 0u) {
        Nds3dTextureSample result{};
        result.status = Nds3dTextureSampleStatus::NoTexture;
        result.alpha = 31u;
        result.has_texture = false;
        result.transparent = false;
        return result;
    }
    const uint32_t width = UINT32_C(8) <<
        ((texture_parameters >> 20u) & UINT32_C(7));
    const uint32_t height = UINT32_C(8) <<
        ((texture_parameters >> 23u) & UINT32_C(7));
    const uint32_t x = resolve_axis(
        texture_s, width,
        (texture_parameters & (UINT32_C(1) << 16u)) != 0u,
        (texture_parameters & (UINT32_C(1) << 18u)) != 0u);
    const uint32_t y = resolve_axis(
        texture_t, height,
        (texture_parameters & (UINT32_C(1) << 17u)) != 0u,
        (texture_parameters & (UINT32_C(1) << 19u)) != 0u);

    const uint64_t texel_index =
        static_cast<uint64_t>(y) * width + x;
    const uint64_t texture_base =
        static_cast<uint64_t>(texture_parameters & UINT32_C(0xFFFF)) * 8u;
    const uint64_t palette_unit = format == 2u ? 8u : 16u;
    const uint64_t palette_address =
        static_cast<uint64_t>(palette_base & UINT32_C(0x1FFF)) *
        palette_unit;

    if (texture_base > UINT32_MAX ||
        palette_address > UINT32_MAX) {
        return failure(Nds3dTextureSampleStatus::InvalidParameters);
    }

    const auto read_texel_byte = [&](uint64_t relative_offset) noexcept {
        const uint64_t address = texture_base + relative_offset;
        if (address > UINT32_MAX) {
            return ReadResult{Status::AddressOverflow, 0u};
        }
        return read_texture_byte(static_cast<uint32_t>(address));
    };

    if (format == 5u) {
        const uint32_t image_slot = static_cast<uint32_t>(
            texture_base / NdsVramControl::BankSize);
        const uint32_t slot_offset = static_cast<uint32_t>(
            texture_base % NdsVramControl::BankSize);
        if ((image_slot != 0u && image_slot != 2u) ||
            (width > 512u && height > 512u)) {
            return failure(Nds3dTextureSampleStatus::InvalidParameters);
        }

        const uint32_t blocks_per_row = width / 4u;
        const uint64_t block_index =
            static_cast<uint64_t>(y / 4u) * blocks_per_row + (x / 4u);
        const uint64_t block_offset =
            static_cast<uint64_t>(slot_offset) + block_index * 4u;
        if (block_offset + 4u > NdsVramControl::BankSize) {
            return failure(Nds3dTextureSampleStatus::InvalidParameters);
        }

        uint32_t block_texels = 0u;
        for (uint32_t byte = 0u; byte < 4u; ++byte) {
            const ReadResult part = vram_.read_texture_slot_byte(
                static_cast<uint8_t>(image_slot),
                static_cast<uint32_t>(block_offset + byte));
            if (part.status != Status::Ok) {
                return failure(map_vram_status(part.status));
            }
            block_texels |=
                (part.value & UINT32_C(0xFF)) << (byte * 8u);
        }

        /*
         * The metadata for Slot 0 is in the lower half of texture Slot 1;
         * Slot 2 uses the upper half. Every four-byte texel block has a
         * corresponding 16-bit palette-index/mode entry.
         */
        const uint64_t metadata_offset =
            (image_slot == 2u ? UINT64_C(0x10000) : 0u) +
            (slot_offset / 2u) + block_index * 2u;
        const uint64_t metadata_limit =
            image_slot == 2u ? UINT64_C(0x20000) : UINT64_C(0x10000);
        const uint64_t metadata_begin =
            image_slot == 2u ? UINT64_C(0x10000) : UINT64_C(0);
        if (metadata_offset < metadata_begin ||
            metadata_offset + 2u > metadata_limit) {
            return failure(Nds3dTextureSampleStatus::InvalidParameters);
        }
        const ReadResult metadata_low = vram_.read_texture_slot_byte(
            1u, static_cast<uint32_t>(metadata_offset));
        const ReadResult metadata_high = vram_.read_texture_slot_byte(
            1u, static_cast<uint32_t>(metadata_offset + 1u));
        if (metadata_low.status != Status::Ok ||
            metadata_high.status != Status::Ok) {
            const Status failed_status =
                metadata_low.status != Status::Ok
                    ? metadata_low.status : metadata_high.status;
            return failure(map_vram_status(failed_status));
        }
        const uint32_t metadata =
            (metadata_low.value & UINT32_C(0xFF)) |
            ((metadata_high.value & UINT32_C(0xFF)) << 8u);
        const uint32_t palette_index_base =
            (metadata & UINT32_C(0x3FFF)) * 4u;
        const uint8_t interpolation_mode = static_cast<uint8_t>(
            metadata >> 14u);
        const uint32_t local_texel =
            ((y & 3u) * 4u) + (x & 3u);
        const uint8_t texel_index_2bpp = static_cast<uint8_t>(
            (block_texels >> (local_texel * 2u)) & UINT32_C(3));

        if ((interpolation_mode == 0u || interpolation_mode == 1u) &&
            texel_index_2bpp == 3u) {
            Nds3dTextureSample transparent{};
            transparent.status = Nds3dTextureSampleStatus::Ok;
            transparent.alpha = 0u;
            transparent.has_texture = true;
            transparent.transparent = true;
            return transparent;
        }

        const uint64_t palette_block_address =
            palette_address + palette_index_base;
        if (palette_block_address > UINT32_MAX) {
            return failure(Nds3dTextureSampleStatus::InvalidParameters);
        }
        const auto read_compressed_color = [&](
            uint8_t color_index) noexcept {
            const uint64_t address = palette_block_address +
                static_cast<uint64_t>(color_index) * 2u;
            if (address > UINT32_MAX) {
                return ReadResult{Status::AddressOverflow, 0u};
            }
            return vram_.read_texture_palette_halfword(
                static_cast<uint32_t>(address));
        };

        const auto sample_from_color = [](
            uint16_t color) noexcept {
            Nds3dTextureSample result{};
            result.status = Nds3dTextureSampleStatus::Ok;
            result.color_rgb555 = static_cast<uint16_t>(
                color & UINT16_C(0x7FFF));
            result.alpha = 31u;
            result.has_texture = true;
            result.transparent = false;
            return result;
        };

        if (texel_index_2bpp <= 1u ||
            interpolation_mode == 0u ||
            interpolation_mode == 2u) {
            const ReadResult selected = read_compressed_color(
                texel_index_2bpp);
            if (selected.status != Status::Ok) {
                return failure(map_vram_status(selected.status));
            }
            return sample_from_color(static_cast<uint16_t>(selected.value));
        }

        const ReadResult color0 = read_compressed_color(0u);
        const ReadResult color1 = read_compressed_color(1u);
        if (color0.status != Status::Ok || color1.status != Status::Ok) {
            const Status failed_status = color0.status != Status::Ok
                ? color0.status : color1.status;
            return failure(map_vram_status(failed_status));
        }

        const uint32_t weight0 =
            interpolation_mode == 1u
                ? 4u
                : (texel_index_2bpp == 2u ? 5u : 3u);
        const uint32_t weight1 = 8u - weight0;
        const uint16_t first = static_cast<uint16_t>(color0.value);
        const uint16_t second = static_cast<uint16_t>(color1.value);
        uint16_t blended = 0u;
        constexpr std::array<uint32_t, 3> channel_shifts{
            0u, 5u, 10u
        };
        for (uint32_t shift : channel_shifts) {
            const uint32_t first_channel =
                (static_cast<uint32_t>(first) >> shift) & UINT32_C(0x1F);
            const uint32_t second_channel =
                (static_cast<uint32_t>(second) >> shift) & UINT32_C(0x1F);
            const uint32_t channel =
                (first_channel * weight0 + second_channel * weight1) / 8u;
            blended = static_cast<uint16_t>(
                blended | static_cast<uint16_t>(channel << shift));
        }
        return sample_from_color(blended);
    }

    if (format == 7u) {
        const uint64_t address =
            texture_base + texel_index * 2u;
        if (address > UINT32_MAX) {
            return failure(Nds3dTextureSampleStatus::InvalidParameters);
        }
        const ReadResult texel =
            read_texture_halfword(static_cast<uint32_t>(address));
        if (texel.status != Status::Ok) {
            return failure(map_vram_status(texel.status));
        }

        const bool opaque = (texel.value & UINT32_C(0x8000)) != 0u;
        Nds3dTextureSample result{};
        result.status = Nds3dTextureSampleStatus::Ok;
        result.color_rgb555 = static_cast<uint16_t>(
            texel.value & UINT32_C(0x7FFF));
        result.alpha = opaque ? 31u : 0u;
        result.has_texture = true;
        result.transparent = !opaque;
        return result;
    }

    uint32_t palette_index = 0u;
    uint8_t alpha = 31u;
    if (format == 2u || format == 3u) {
        const uint32_t bits_per_texel = format == 2u ? 2u : 4u;
        const uint32_t texels_per_byte = 8u / bits_per_texel;
        const uint64_t byte_offset =
            (texel_index * bits_per_texel) / 8u;
        const ReadResult packed = read_texel_byte(byte_offset);
        if (packed.status != Status::Ok) {
            return failure(map_vram_status(packed.status));
        }
        const uint32_t shift = static_cast<uint32_t>(
            (texel_index % texels_per_byte) * bits_per_texel);
        palette_index = static_cast<uint32_t>(
            (packed.value >> shift) &
            ((UINT32_C(1) << bits_per_texel) - 1u));
    } else {
        const ReadResult texel = read_texel_byte(texel_index);
        if (texel.status != Status::Ok) {
            return failure(map_vram_status(texel.status));
        }
        const uint8_t value = static_cast<uint8_t>(texel.value);
        if (format == 1u) {
            palette_index = value & UINT8_C(0x1F);
            const uint8_t alpha3 = static_cast<uint8_t>(value >> 5u);
            alpha = static_cast<uint8_t>(
                alpha3 * 4u + alpha3 / 2u);
        } else if (format == 4u) {
            palette_index = value;
        } else if (format == 6u) {
            palette_index = value & UINT8_C(0x07);
            alpha = static_cast<uint8_t>(value >> 3u);
        } else {
            return failure(Nds3dTextureSampleStatus::InvalidParameters);
        }
    }

    if ((texture_parameters & (UINT32_C(1) << 29u)) != 0u &&
        (format == 2u || format == 3u || format == 4u) &&
        palette_index == 0u) {
        alpha = 0u;
    }

    const uint64_t color_address =
        palette_address + static_cast<uint64_t>(palette_index) * 2u;
    if (color_address > UINT32_MAX) {
        return failure(Nds3dTextureSampleStatus::InvalidParameters);
    }
    return read_palette_color(
        static_cast<uint32_t>(color_address), alpha);
}

} // namespace oracis::memory
