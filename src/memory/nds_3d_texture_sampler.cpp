#include "oracis/memory/nds_3d_texture_sampler.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

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
    if (format == 5u) {
        /* 4x4 block metadata is decoded in a separate follow-up stage. */
        return failure(Nds3dTextureSampleStatus::UnsupportedFormat);
    }
    if (format > 7u) {
        return failure(Nds3dTextureSampleStatus::InvalidParameters);
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
