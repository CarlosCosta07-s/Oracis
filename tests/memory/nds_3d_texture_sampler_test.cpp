#include "oracis/memory/guest_memory.hpp"
#include "oracis/memory/nds_3d_texture_sampler.hpp"
#include "oracis/memory/nds_vram_control.hpp"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_3d_texture_sampler_test: %s\n", message);
    return 1;
}

bool write_halfword(
    oracis::memory::GuestMemory &memory,
    uint32_t address,
    uint16_t value)
{
    return memory.write(NDS_ARM9, address, 2u, value) ==
        oracis::memory::Status::Ok;
}

} // namespace

int main()
{
    using namespace oracis::memory;

    GuestMemory memory;
    NdsVramControl vram(memory);
    if (vram.install() != Status::Ok) {
        return fail("VRAM controller installation failed");
    }

    /* Populate texture and palette banks through their temporary LCDC views. */
    if (vram.write(
            NDS_ARM9, NdsVramControl::VramCntAAddress, 1u, UINT32_C(0x80)) !=
            Status::Ok ||
        vram.write(
            NDS_ARM9, NdsVramControl::VramCntEAddress, 1u, UINT32_C(0x80)) !=
            Status::Ok) {
        return fail("temporary LCDC bank allocation failed");
    }

    /* Slot-0 texture fixtures: indexed, translucent and direct-color formats. */
    if (!write_halfword(memory, UINT32_C(0x06800080), UINT16_C(0x00E1)) ||
        !write_halfword(memory, UINT32_C(0x06800100), UINT16_C(0x801F)) ||
        !write_halfword(memory, UINT32_C(0x06800102), UINT16_C(0x001F)) ||
        !write_halfword(memory, UINT32_C(0x0680010E), UINT16_C(0x83E0)) ||
        !write_halfword(memory, UINT32_C(0x06800180), UINT16_C(0x0002)) ||
        !write_halfword(memory, UINT32_C(0x06800200), UINT16_C(0x0003)) ||
        !write_halfword(memory, UINT32_C(0x06800280), UINT16_C(0x0004)) ||
        !write_halfword(memory, UINT32_C(0x06800300), UINT16_C(0x00FB))) {
        return fail("synthetic texture data could not be written");
    }

    /* Palette values are RGB555: index 1 green, index 2 blue. */
    if (!write_halfword(memory, UINT32_C(0x06880022), UINT16_C(0x03E0)) ||
        !write_halfword(memory, UINT32_C(0x0688000C), UINT16_C(0x7C00)) ||
        !write_halfword(memory, UINT32_C(0x06880036), UINT16_C(0x03FF)) ||
        !write_halfword(memory, UINT32_C(0x06880048), UINT16_C(0x7FE0)) ||
        !write_halfword(memory, UINT32_C(0x06890006), UINT16_C(0x4210))) {
        return fail("synthetic texture palette could not be written");
    }

    if (vram.write(
            NDS_ARM9, NdsVramControl::VramCntAAddress, 1u, UINT32_C(0x83)) !=
            Status::Ok ||
        vram.write(
            NDS_ARM9, NdsVramControl::VramCntEAddress, 1u, UINT32_C(0x83)) !=
            Status::Ok ||
        vram.write(
            NDS_ARM9, NdsVramControl::VramCntFAddress, 1u, UINT32_C(0x93)) !=
            Status::Ok) {
        return fail("texture image/palette allocation failed");
    }

    Nds3dTextureSampler sampler(vram);

    /* A3I5: index 1, alpha 7/7 -> expanded alpha 31/31. */
    const uint32_t a3i5 = (UINT32_C(1) << 26u) | UINT32_C(0x10);
    const Nds3dTextureSample a3i5_sample =
        sampler.sample(a3i5, UINT32_C(2), 0, 0);
    if (a3i5_sample.status != Nds3dTextureSampleStatus::Ok ||
        !a3i5_sample.has_texture || a3i5_sample.transparent ||
        a3i5_sample.color_rgb555 != UINT16_C(0x03E0) ||
        a3i5_sample.alpha != 31u) {
        return fail("A3I5 palette lookup or alpha expansion failed");
    }

    /* 2bpp: first texel index 2 uses an 8-byte PLTT_BASE unit. */
    const uint32_t palette2 = (UINT32_C(2) << 26u) | UINT32_C(0x30);
    const Nds3dTextureSample palette2_sample =
        sampler.sample(palette2, UINT32_C(1), 0, 0);
    if (palette2_sample.status != Nds3dTextureSampleStatus::Ok ||
        palette2_sample.color_rgb555 != UINT16_C(0x7C00) ||
        palette2_sample.alpha != 31u) {
        return fail("2bpp texel packing or 8-byte palette base failed");
    }

    /* TEXIMAGE_PARAM bit 29 makes palette index zero transparent. */
    const Nds3dTextureSample color_zero =
        sampler.sample(
            palette2 | (UINT32_C(1) << 29u), UINT32_C(1), 16, 0);
    if (color_zero.status != Nds3dTextureSampleStatus::Ok ||
        !color_zero.transparent || color_zero.alpha != 0u) {
        return fail("transparent palette color-zero handling failed");
    }

    /* 4bpp uses the low nibble for the first texel. */
    const uint32_t palette4bpp =
        (UINT32_C(3) << 26u) | UINT32_C(0x40);
    const Nds3dTextureSample palette4bpp_sample =
        sampler.sample(palette4bpp, UINT32_C(3), 0, 0);
    if (palette4bpp_sample.status != Nds3dTextureSampleStatus::Ok ||
        palette4bpp_sample.color_rgb555 != UINT16_C(0x03FF)) {
        return fail("4bpp nibble ordering or palette lookup failed");
    }

    /* 8bpp indexes a 256-entry palette using 16-byte PLTT_BASE units. */
    const uint32_t palette8bpp =
        (UINT32_C(4) << 26u) | UINT32_C(0x50);
    const Nds3dTextureSample palette8bpp_sample =
        sampler.sample(palette8bpp, UINT32_C(4), 0, 0);
    if (palette8bpp_sample.status != Nds3dTextureSampleStatus::Ok ||
        palette8bpp_sample.color_rgb555 != UINT16_C(0x7FE0)) {
        return fail("8bpp palette index lookup failed");
    }

    /* A5I3 uses a five-bit per-texel alpha and can read palette slot 4. */
    const uint32_t a5i3 = (UINT32_C(6) << 26u) | UINT32_C(0x60);
    const Nds3dTextureSample a5i3_sample =
        sampler.sample(a5i3, UINT32_C(0x1000), 0, 0);
    if (a5i3_sample.status != Nds3dTextureSampleStatus::Ok ||
        a5i3_sample.color_rgb555 != UINT16_C(0x4210) ||
        a5i3_sample.alpha != 31u) {
        return fail("A5I3 alpha or F-bank palette slot mapping failed");
    }

    /* Direct texture: bit 15 supplies per-texel alpha. */
    const uint32_t direct = (UINT32_C(7) << 26u) | UINT32_C(0x20);
    const Nds3dTextureSample direct_opaque =
        sampler.sample(direct, 0u, 0, 0);
    const Nds3dTextureSample direct_transparent =
        sampler.sample(direct, 0u, 16, 0);
    if (direct_opaque.status != Nds3dTextureSampleStatus::Ok ||
        direct_opaque.color_rgb555 != UINT16_C(0x001F) ||
        direct_opaque.alpha != 31u ||
        direct_transparent.status != Nds3dTextureSampleStatus::Ok ||
        !direct_transparent.transparent ||
        direct_transparent.alpha != 0u) {
        return fail("direct-color RGB555/alpha decoding failed");
    }

    /* Repeat+flip maps the second texture period back from the far edge. */
    const uint32_t repeat_flip =
        direct | (UINT32_C(1) << 16u) | (UINT32_C(1) << 18u);
    const Nds3dTextureSample flipped =
        sampler.sample(repeat_flip, 0u, 128, 0);
    if (flipped.status != Nds3dTextureSampleStatus::Ok ||
        flipped.color_rgb555 != UINT16_C(0x03E0) ||
        flipped.alpha != 31u) {
        return fail("repeat/flip coordinate addressing failed");
    }

    const Nds3dTextureSample no_texture =
        sampler.sample(0u, 0u, 0, 0);
    if (no_texture.status != Nds3dTextureSampleStatus::NoTexture ||
        no_texture.has_texture) {
        return fail("no-texture format was not reported distinctly");
    }

    const Nds3dTextureSample compressed =
        sampler.sample(
            (UINT32_C(5) << 26u), 0u, 0, 0);
    if (compressed.status != Nds3dTextureSampleStatus::UnsupportedFormat) {
        return fail("compressed format did not fail closed");
    }

    if (vram.write(
            NDS_ARM9, NdsVramControl::VramCntAAddress, 1u, 0u) != Status::Ok) {
        return fail("texture bank disable failed");
    }
    const Nds3dTextureSample unavailable =
        sampler.sample(direct, 0u, 0, 0);
    if (unavailable.status != Nds3dTextureSampleStatus::UnmappedVram) {
        return fail("unallocated texture slot was sampled");
    }

    std::puts("NDS 3D texture sampling tests passed");
    return 0;
}
