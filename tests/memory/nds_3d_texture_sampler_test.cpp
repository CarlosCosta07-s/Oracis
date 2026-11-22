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
            NDS_ARM9, NdsVramControl::VramCntBAddress, 1u, UINT32_C(0x80)) !=
            Status::Ok ||
        vram.write(
            NDS_ARM9, NdsVramControl::VramCntCAddress, 1u, UINT32_C(0x80)) !=
            Status::Ok ||
        vram.write(
            NDS_ARM9, NdsVramControl::VramCntDAddress, 1u, UINT32_C(0x80)) !=
            Status::Ok ||
        vram.write(
            NDS_ARM9, NdsVramControl::VramCntEAddress, 1u, UINT32_C(0x80)) !=
            Status::Ok ||
        vram.write(
            NDS_ARM9, NdsVramControl::VramCntFAddress, 1u, UINT32_C(0x80)) !=
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
        !write_halfword(memory, UINT32_C(0x06800300), UINT16_C(0x00FB)) ||
        !write_halfword(memory, UINT32_C(0x06804000), UINT16_C(0x000E)) ||
        !write_halfword(memory, UINT32_C(0x06804002), UINT16_C(0x0000)) ||
        !write_halfword(memory, UINT32_C(0x06804004), UINT16_C(0x000E)) ||
        !write_halfword(memory, UINT32_C(0x06804006), UINT16_C(0x0000)) ||
        !write_halfword(memory, UINT32_C(0x06804008), UINT16_C(0x000F)) ||
        !write_halfword(memory, UINT32_C(0x0680400A), UINT16_C(0x0000)) ||
        !write_halfword(memory, UINT32_C(0x0680400C), UINT16_C(0x000E)) ||
        !write_halfword(memory, UINT32_C(0x0680400E), UINT16_C(0x0000)) ||
        !write_halfword(memory, UINT32_C(0x06840000), UINT16_C(0x000E)) ||
        !write_halfword(memory, UINT32_C(0x06840002), UINT16_C(0x0000)) ||
        !write_halfword(memory, UINT32_C(0x06830000), UINT16_C(0x4000)) ||
        !write_halfword(memory, UINT32_C(0x06860000), UINT16_C(0xBEEF))) {
        return fail("synthetic texture data could not be written");
    }

    /* Palette values are RGB555: index 1 green, index 2 blue. */
    if (!write_halfword(memory, UINT32_C(0x06880022), UINT16_C(0x03E0)) ||
        !write_halfword(memory, UINT32_C(0x0688000C), UINT16_C(0x7C00)) ||
        !write_halfword(memory, UINT32_C(0x06880036), UINT16_C(0x03FF)) ||
        !write_halfword(memory, UINT32_C(0x06880048), UINT16_C(0x7FE0)) ||
        !write_halfword(memory, UINT32_C(0x06880000), UINT16_C(0x001F)) ||
        !write_halfword(memory, UINT32_C(0x06880002), UINT16_C(0x7C00)) ||
        !write_halfword(memory, UINT32_C(0x06880004), UINT16_C(0x7FFF)) ||
        !write_halfword(memory, UINT32_C(0x06880006), UINT16_C(0x03E0)) ||
        !write_halfword(memory, UINT32_C(0x06890006), UINT16_C(0x4210)) ||
        !write_halfword(memory, UINT32_C(0x06822000), UINT16_C(0x4000)) ||
        !write_halfword(memory, UINT32_C(0x06822002), UINT16_C(0xC000)) ||
        !write_halfword(memory, UINT32_C(0x06822004), UINT16_C(0x8000)) ||
        !write_halfword(memory, UINT32_C(0x06822006), UINT16_C(0x0000))) {
        return fail("synthetic texture palette could not be written");
    }

    if (vram.write(
            NDS_ARM9, NdsVramControl::VramCntAAddress, 1u, UINT32_C(0x83)) !=
            Status::Ok ||
        vram.write(
            NDS_ARM9, NdsVramControl::VramCntBAddress, 1u, UINT32_C(0x8B)) !=
            Status::Ok ||
        vram.write(
            NDS_ARM9, NdsVramControl::VramCntCAddress, 1u, UINT32_C(0x93)) !=
            Status::Ok ||
        vram.write(
            NDS_ARM9, NdsVramControl::VramCntDAddress, 1u, UINT32_C(0x9B)) !=
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

    const ReadResult slot2_word =
        sampler.read_texture_slot_halfword(2u, 0u);
    const ReadResult slot3_word =
        sampler.read_texture_slot_halfword(3u, 0u);
    if (slot2_word.status != Status::Ok ||
        slot2_word.value != UINT16_C(0x000E) ||
        slot3_word.status != Status::Ok ||
        slot3_word.value != UINT16_C(0xBEEF) ||
        sampler.read_texture_slot_halfword(2u, 1u).status !=
            Status::InvalidRegion ||
        sampler.read_texture_slot_halfword(
            3u, NdsVramControl::BankSize - 1u).status !=
            Status::InvalidRegion ||
        sampler.read_texture_slot_halfword(
            3u, NdsVramControl::BankSize - 2u).status != Status::Ok) {
        return fail("texture-slot halfword reads or bounds validation failed");
    }

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

    /* Overlapping F/E palette slot assignments fail closed. */
    if (vram.write(
            NDS_ARM9, NdsVramControl::VramCntFAddress, 1u,
            UINT32_C(0x83)) != Status::Ok) {
        return fail("overlapping palette slot fixture could not be configured");
    }
    const Nds3dTextureSample ambiguous_palette =
        sampler.sample(a3i5, UINT32_C(2), 0, 0);
    if (ambiguous_palette.status !=
            Nds3dTextureSampleStatus::AmbiguousVram ||
        vram.write(
            NDS_ARM9, NdsVramControl::VramCntFAddress, 1u,
            UINT32_C(0x93)) != Status::Ok) {
        return fail("overlapping texture-palette allocation was not rejected");
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

    /* Two image banks claiming slot 0 must not be chosen arbitrarily. */
    if (vram.write(
            NDS_ARM9, NdsVramControl::VramCntBAddress, 1u,
            UINT32_C(0x83)) != Status::Ok) {
        return fail("overlapping texture image slot fixture could not be configured");
    }
    const Nds3dTextureSample ambiguous_texture =
        sampler.sample(direct, 0u, 0, 0);
    if (ambiguous_texture.status !=
            Nds3dTextureSampleStatus::AmbiguousVram ||
        vram.write(
            NDS_ARM9, NdsVramControl::VramCntBAddress, 1u,
            UINT32_C(0x8B)) != Status::Ok) {
        return fail("overlapping texture-image allocation was not rejected");
    }

    const Nds3dTextureSample no_texture =
        sampler.sample(0u, 0u, 0, 0);
    if (no_texture.status != Nds3dTextureSampleStatus::NoTexture ||
        no_texture.has_texture) {
        return fail("no-texture format was not reported distinctly");
    }

    /*
     * 4x4 compressed textures use packed 2bpp blocks in slot 0 and
     * per-block metadata in the lower half of texture slot 1.
     */
    const uint32_t compressed_parameters =
        (UINT32_C(5) << 26u) | UINT32_C(0x800);
    const Nds3dTextureSample compressed_mode1_color2 =
        sampler.sample(compressed_parameters, 0u, 0, 0);
    const Nds3dTextureSample compressed_mode1_transparent =
        sampler.sample(compressed_parameters, 0u, 16, 0);
    if (compressed_mode1_color2.status != Nds3dTextureSampleStatus::Ok ||
        compressed_mode1_color2.color_rgb555 !=
            static_cast<uint16_t>(UINT16_C(15) | (UINT16_C(15) << 10u)) ||
        compressed_mode1_transparent.status != Nds3dTextureSampleStatus::Ok ||
        !compressed_mode1_transparent.transparent ||
        compressed_mode1_transparent.alpha != 0u) {
        return fail("4x4 compressed mode-1 interpolation/transparency failed");
    }

    /* Mode 0 selects the third palette color directly; code 3 is transparent. */
    const Nds3dTextureSample compressed_mode0_color2 =
        sampler.sample(compressed_parameters, 0u, 64, 64);
    const Nds3dTextureSample compressed_mode0_transparent =
        sampler.sample(compressed_parameters, 0u, 80, 64);
    if (compressed_mode0_color2.status != Nds3dTextureSampleStatus::Ok ||
        compressed_mode0_color2.color_rgb555 != UINT16_C(0x7FFF) ||
        compressed_mode0_transparent.status != Nds3dTextureSampleStatus::Ok ||
        !compressed_mode0_transparent.transparent) {
        return fail("4x4 compressed mode-0 color selection/transparency failed");
    }

    /* Slot 2 uses the upper half of slot 1 for its block metadata. */
    const uint32_t compressed_slot2_parameters =
        (UINT32_C(5) << 26u) | UINT32_C(0x8000);
    const Nds3dTextureSample compressed_slot2 =
        sampler.sample(compressed_slot2_parameters, 0u, 0, 0);
    if (compressed_slot2.status != Nds3dTextureSampleStatus::Ok ||
        compressed_slot2.color_rgb555 !=
            static_cast<uint16_t>(UINT16_C(15) | (UINT16_C(15) << 10u))) {
        return fail("4x4 compressed slot-2 metadata mapping failed");
    }

    /* Mode 2 keeps texel 3 opaque and addresses all four palette colors. */
    const Nds3dTextureSample compressed_mode2_color3 =
        sampler.sample(compressed_parameters, 0u, 0, 64);
    if (compressed_mode2_color3.status != Nds3dTextureSampleStatus::Ok ||
        compressed_mode2_color3.color_rgb555 != UINT16_C(0x03E0) ||
        compressed_mode2_color3.transparent) {
        return fail("4x4 compressed mode-2 fourth palette color failed");
    }

    /* Mode 3 uses the documented 5:3 and 3:5 weighted colors. */
    const Nds3dTextureSample compressed_mode3_color2 =
        sampler.sample(compressed_parameters, 0u, 64, 0);
    const Nds3dTextureSample compressed_mode3_color3 =
        sampler.sample(compressed_parameters, 0u, 80, 0);
    if (compressed_mode3_color2.status != Nds3dTextureSampleStatus::Ok ||
        compressed_mode3_color2.color_rgb555 !=
            static_cast<uint16_t>(UINT16_C(19) | (UINT16_C(11) << 10u)) ||
        compressed_mode3_color3.status != Nds3dTextureSampleStatus::Ok ||
        compressed_mode3_color3.color_rgb555 !=
            static_cast<uint16_t>(UINT16_C(11) | (UINT16_C(19) << 10u))) {
        return fail("4x4 compressed mode-3 weighted colors failed");
    }

    if (vram.write(
            NDS_ARM9, NdsVramControl::VramCntDAddress, 1u, 0u) != Status::Ok ||
        sampler.read_texture_slot_halfword(3u, 0u).status != Status::Unmapped) {
        return fail("unallocated rear-plane/depth slot was sampled");
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
