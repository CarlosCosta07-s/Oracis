/*
 * Nintendo DS 3D texture texel sampler.
 *
 * This stage resolves integer texel coordinates and decodes the uncompressed
 * texture formats. Polygon interpolation, compressed 4x4 format, filtering,
 * texture caching and fragment composition remain separate stages.
 */
#ifndef ORACIS_MEMORY_NDS_3D_TEXTURE_SAMPLER_HPP
#define ORACIS_MEMORY_NDS_3D_TEXTURE_SAMPLER_HPP

#include <cstdint>

#include "oracis/memory/nds_vram_control.hpp"

namespace oracis::memory {

enum class Nds3dTextureSampleStatus : uint8_t {
    Ok = 0,
    NoTexture,
    UnsupportedFormat,
    InvalidParameters,
    UnmappedVram,
    AmbiguousVram
};

struct Nds3dTextureSample {
    Nds3dTextureSampleStatus status = Nds3dTextureSampleStatus::InvalidParameters;
    uint16_t color_rgb555 = 0u;
    uint8_t alpha = 0u;
    bool has_texture = false;
    bool transparent = true;
};

class Nds3dTextureSampler final {
public:
    explicit Nds3dTextureSampler(const NdsVramControl &vram) noexcept
        : vram_(vram) {}

    Nds3dTextureSampler(const Nds3dTextureSampler &) = delete;
    Nds3dTextureSampler &operator=(const Nds3dTextureSampler &) = delete;

    /*
     * Sample the nearest texel at signed 1.11.4 coordinates.
     * texture_parameters is the captured TEXIMAGE_PARAM command value;
     * palette_base is the captured PLTT_BASE value (not a byte address).
     */
    Nds3dTextureSample sample(
        uint32_t texture_parameters,
        uint32_t palette_base,
        int16_t texture_s,
        int16_t texture_t) const noexcept;

    ReadResult read_texture_slot_halfword(
        uint8_t texture_slot, uint32_t byte_offset) const noexcept;

private:
    struct TexelCoordinate {
        uint32_t x = 0u;
        uint32_t y = 0u;
    };

    static int32_t floor_divide(int32_t value, int32_t divisor) noexcept;
    static uint32_t resolve_axis(
        int16_t coordinate, uint32_t size,
        bool repeat, bool flip) noexcept;

    ReadResult read_texture_byte(uint32_t byte_address) const noexcept;
    ReadResult read_texture_halfword(uint32_t byte_address) const noexcept;
    Nds3dTextureSample read_palette_color(
        uint32_t byte_address, uint8_t alpha) const noexcept;

    const NdsVramControl &vram_;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_3D_TEXTURE_SAMPLER_HPP */
