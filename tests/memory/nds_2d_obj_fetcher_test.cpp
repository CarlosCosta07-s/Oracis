#include "oracis/memory/nds_2d_obj_fetcher.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"
#include "oracis/memory/nds_video_memory.hpp"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_2d_obj_fetcher_test: %s\n", message);
    return 1;
}

bool write16(
    oracis::memory::GuestMemory &memory, uint32_t address, uint16_t value)
{
    return memory.write(NDS_ARM9, address, 2u, value) ==
        oracis::memory::Status::Ok;
}

bool write32(
    oracis::memory::GuestMemory &memory, uint32_t address, uint32_t value)
{
    return memory.write(NDS_ARM9, address, 4u, value) ==
        oracis::memory::Status::Ok;
}

bool write_oam(
    oracis::memory::GuestMemory &memory,
    unsigned engine,
    unsigned object,
    uint16_t attr0,
    uint16_t attr1,
    uint16_t attr2)
{
    const uint32_t base = UINT32_C(0x07000000) +
        (engine == 0u ? 0u : UINT32_C(0x400)) + object * 8u;
    return write16(memory, base, attr0) &&
        write16(memory, base + 2u, attr1) &&
        write16(memory, base + 4u, attr2);
}

bool write_affine_matrix(
    oracis::memory::GuestMemory &memory,
    unsigned engine,
    unsigned matrix,
    uint16_t pa,
    uint16_t pb,
    uint16_t pc,
    uint16_t pd)
{
    const uint32_t base = UINT32_C(0x07000000) +
        (engine == 0u ? 0u : UINT32_C(0x400)) + matrix * 32u;
    return write16(memory, base + 6u, pa) &&
        write16(memory, base + 14u, pb) &&
        write16(memory, base + 22u, pc) &&
        write16(memory, base + 30u, pd);
}

} // namespace

int main()
{
    using namespace oracis::memory;

    GuestMemory memory;
    NdsVideoMemory video_memory(memory);
    if (video_memory.install() != Status::Ok ||
        memory.map_ram(
            UINT32_C(0x06400000), UINT32_C(0x00040000),
            static_cast<uint8_t>(Readable | Writable),
            "test Main OBJ VRAM", CpuAccessArm9) != Status::Ok ||
        memory.map_ram(
            UINT32_C(0x06600000), UINT32_C(0x00020000),
            static_cast<uint8_t>(Readable | Writable),
            "test Sub OBJ VRAM", CpuAccessArm9) != Status::Ok) {
        return fail("OBJ/OAM/palette memory setup failed");
    }

    NdsInterruptControl interrupts(memory);
    NdsDisplayControl display(interrupts);
    if (display.install() != Status::Ok) {
        return fail("display register device failed to install");
    }
    Nds2dObjFetcher fetcher(memory);
    Nds2dObjScanline line{};

    /* OBJ0: 8x8, 4bpp, x=5, y=10, priority 1, 2D character mapping. */
    if (!write_oam(
            memory, 0u, 0u,
            UINT16_C(0x000A),
            UINT16_C(0x0005),
            UINT16_C(0x0400)) ||
        !write32(memory, UINT32_C(0x06400000), UINT32_C(0x87654321)) ||
        !write16(memory, UINT32_C(0x05000202), UINT16_C(0x001F)) ||
        !write16(memory, UINT32_C(0x05000204), UINT16_C(0x03E0)) ||
        !write16(memory, UINT32_C(0x05000206), UINT16_C(0x7C00)) ||
        !write16(memory, UINT32_C(0x05000208), UINT16_C(0x4210)) ||
        !write16(memory, UINT32_C(0x0500020A), UINT16_C(0x2108)) ||
        !write16(memory, UINT32_C(0x0500020C), UINT16_C(0x6318)) ||
        !write16(memory, UINT32_C(0x0500020E), UINT16_C(0x56B5)) ||
        !write16(memory, UINT32_C(0x05000210), UINT16_C(0x294A)) ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            UINT32_C(1) << 12u) != Status::Ok) {
        return fail("4bpp OBJ0 fixture setup failed");
    }

    auto result = fetcher.render_scanline(0u, display, 10u, line);
    if (result.status != Nds2dObjFetchStatus::Ok ||
        line.pixels[5].transparent ||
        line.pixels[5].rgb555 != UINT16_C(0x001F) ||
        line.pixels[6].rgb555 != UINT16_C(0x03E0) ||
        line.pixels[12].rgb555 != UINT16_C(0x294A) ||
        !line.pixels[13].transparent ||
        line.pixels[5].object_index != 0u ||
        line.pixels[5].priority != 1u) {
        return fail("4bpp tile/palette/transparent-index fetch failed");
    }

    if (!write_oam(
            memory, 0u, 0u,
            UINT16_C(0x000A),
            UINT16_C(0x1005),
            UINT16_C(0x0400))) {
        return fail("OBJ horizontal-flip fixture update failed");
    }
    result = fetcher.render_scanline(0u, display, 10u, line);
    if (result.status != Nds2dObjFetchStatus::Ok ||
        line.pixels[5].rgb555 != UINT16_C(0x294A) ||
        line.pixels[12].rgb555 != UINT16_C(0x001F)) {
        return fail("OBJ horizontal flip did not reverse tile sampling");
    }

    /* Equal priority preserves the lower OAM index; priority bits still dominate. */
    if (!write_oam(
            memory, 0u, 1u,
            UINT16_C(0x000A),
            UINT16_C(0x0005),
            UINT16_C(0x0400)) ||
        !write32(memory, UINT32_C(0x06400020), UINT32_C(0x11111111)) ||
        !write16(memory, UINT32_C(0x05000202), UINT16_C(0x7C00))) {
        return fail("OBJ overlap fixture setup failed");
    }
    result = fetcher.render_scanline(0u, display, 10u, line);
    if (result.status != Nds2dObjFetchStatus::Ok ||
        line.pixels[5].object_index != 0u ||
        line.pixels[5].rgb555 != UINT16_C(0x294A)) {
        return fail("equal-priority OAM index ordering failed");
    }
    if (!write_oam(
            memory, 0u, 0u,
            UINT16_C(0x000A),
            UINT16_C(0x1005),
            UINT16_C(0x0800)) ||
        !write_oam(
            memory, 0u, 1u,
            UINT16_C(0x000A),
            UINT16_C(0x0005),
            UINT16_C(0x0C00))) {
        return fail("OBJ priority fixture update failed");
    }
    result = fetcher.render_scanline(0u, display, 10u, line);
    if (result.status != Nds2dObjFetchStatus::Ok ||
        line.pixels[5].object_index != 0u ||
        line.pixels[5].priority != 2u) {
        return fail("OBJ priority field did not override OAM tie ordering");
    }

    /* 1D 8bpp mapping starts at TileNumber * 32 and advances 64 bytes/tile. */
    if (!write_oam(
            memory, 0u, 0u,
            UINT16_C(0x600A),
            UINT16_C(0x0000),
            UINT16_C(0x0002)) ||
        !write_oam(
            memory, 0u, 1u,
            UINT16_C(0x020A),
            UINT16_C(0x0000),
            UINT16_C(0x0000)) ||
        !write32(memory, UINT32_C(0x06400040), UINT32_C(0x01010101)) ||
        !write32(memory, UINT32_C(0x06400044), UINT32_C(0x01010101)) ||
        !write32(memory, UINT32_C(0x06400080), UINT32_C(0x02020202)) ||
        !write32(memory, UINT32_C(0x06400084), UINT32_C(0x02020202)) ||
        !write16(memory, UINT32_C(0x05000202), UINT16_C(0x001F)) ||
        !write16(memory, UINT32_C(0x05000204), UINT16_C(0x03E0)) ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            (UINT32_C(1) << 12u) | (UINT32_C(1) << 4u)) != Status::Ok) {
        return fail("1D 8bpp fixture setup failed");
    }
    result = fetcher.render_scanline(0u, display, 10u, line);
    if (result.status != Nds2dObjFetchStatus::Ok ||
        line.pixels[0].rgb555 != UINT16_C(0x001F) ||
        line.pixels[7].rgb555 != UINT16_C(0x001F) ||
        line.pixels[8].rgb555 != UINT16_C(0x03E0) ||
        line.pixels[15].rgb555 != UINT16_C(0x03E0)) {
        return fail("1D 8bpp tile stride or palette lookup failed");
    }

    /* OBJ-window pixels produce a mask and no visible OBJ color pixel. */
    if (!write_oam(
            memory, 0u, 0u,
            UINT16_C(0x0800),
            UINT16_C(0x0000),
            UINT16_C(0x0000)) ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            UINT32_C(1) << 15u) != Status::Ok) {
        return fail("OBJ-window fixture setup failed");
    }
    result = fetcher.render_scanline(0u, display, 0u, line);
    if (result.status != Nds2dObjFetchStatus::Ok ||
        !line.object_window_coverage[0] ||
        !line.pixels[0].transparent) {
        return fail("OBJ-window coverage was not separated from color pixels");
    }

    /*
     * Affine matrix 3 lives in OAM's interleaved parameter slots. Identity
     * mapping must preserve the original 8x8 source texel layout.
     */
    if (!write_oam(
            memory, 0u, 0u,
            UINT16_C(0x010A),
            static_cast<uint16_t>((3u << 9u) | 5u),
            UINT16_C(0x0000)) ||
        !write_affine_matrix(
            memory, 0u, 3u,
            UINT16_C(0x0100), 0u, 0u, UINT16_C(0x0100)) ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            UINT32_C(1) << 12u) != Status::Ok) {
        return fail("identity affine OBJ fixture setup failed");
    }
    result = fetcher.render_scanline(0u, display, 10u, line);
    if (result.status != Nds2dObjFetchStatus::Ok ||
        line.pixels[5].transparent ||
        line.pixels[5].rgb555 != UINT16_C(0x001F) ||
        line.pixels[6].rgb555 != UINT16_C(0x03E0) ||
        line.pixels[12].rgb555 != UINT16_C(0x294A)) {
        return fail("identity affine matrix or interleaved matrix index failed");
    }

    /* PA=0.5 samples two adjacent output pixels from the same source texel. */
    if (!write_affine_matrix(
            memory, 0u, 7u,
            UINT16_C(0x0080), 0u, 0u, UINT16_C(0x0100)) ||
        !write_oam(
            memory, 0u, 0u,
            UINT16_C(0x010A),
            static_cast<uint16_t>((7u << 9u) | 5u),
            UINT16_C(0x0000))) {
        return fail("affine scaling fixture setup failed");
    }
    result = fetcher.render_scanline(0u, display, 10u, line);
    if (result.status != Nds2dObjFetchStatus::Ok ||
        line.pixels[5].rgb555 != UINT16_C(0x7C00) ||
        line.pixels[6].rgb555 != UINT16_C(0x7C00) ||
        line.pixels[7].rgb555 != UINT16_C(0x4210)) {
        return fail("affine horizontal scaling produced incorrect source coordinates");
    }

    /* Negative PB and positive PC exercise signed matrix coefficients. */
    for (uint32_t row = 0u; row < 8u; ++row) {
        if (!write32(
                memory,
                UINT32_C(0x06400000) + row * 4u,
                UINT32_C(0x87654321))) {
            return fail("affine rotation source tile setup failed");
        }
    }
    if (!write_affine_matrix(
            memory, 0u, 8u,
            0u, UINT16_C(0xFF00), UINT16_C(0x0100), 0u) ||
        !write_oam(
            memory, 0u, 0u,
            UINT16_C(0x010A),
            static_cast<uint16_t>((8u << 9u) | 5u),
            UINT16_C(0x0000))) {
        return fail("affine rotation fixture setup failed");
    }
    result = fetcher.render_scanline(0u, display, 13u, line);
    if (result.status != Nds2dObjFetchStatus::Ok ||
        line.pixels[5].rgb555 != UINT16_C(0x6318)) {
        return fail("signed PB/PC affine rotation mapping failed");
    }

    /*
     * Double-size expands the destination area to 16x16 while keeping the
     * source image 8x8 and centered; its first texel appears at local (4,4).
     */
    if (!write_affine_matrix(
            memory, 0u, 9u,
            UINT16_C(0x0100), 0u, 0u, UINT16_C(0x0100)) ||
        !write_oam(
            memory, 0u, 0u,
            UINT16_C(0x0300) | UINT16_C(30),
            static_cast<uint16_t>((9u << 9u) | 20u),
            UINT16_C(0x0000))) {
        return fail("double-size affine fixture setup failed");
    }
    result = fetcher.render_scanline(0u, display, 34u, line);
    if (result.status != Nds2dObjFetchStatus::Ok ||
        line.pixels[24].transparent ||
        line.pixels[24].rgb555 != UINT16_C(0x001F) ||
        !line.pixels[20].transparent) {
        return fail("double-size affine bounding-box center was incorrect");
    }
    result = fetcher.render_scanline(0u, display, 30u, line);
    if (result.status != Nds2dObjFetchStatus::Ok ||
        !line.pixels[24].transparent) {
        return fail("double-size affine transparent border was not clipped");
    }

    /* Engine B uses its own OAM half and separate OBJ tile/palette windows. */
    if (!write_oam(
            memory, 1u, 0u,
            UINT16_C(0x0000),
            UINT16_C(0x0000),
            UINT16_C(0x0000)) ||
        !write32(memory, UINT32_C(0x06600000), UINT32_C(0x11111111)) ||
        !write16(memory, UINT32_C(0x05000602), UINT16_C(0x4567)) ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineBBase, 4u,
            UINT32_C(1) << 12u) != Status::Ok) {
        return fail("Engine B OBJ fixture setup failed");
    }
    result = fetcher.render_scanline(1u, display, 0u, line);
    if (result.status != Nds2dObjFetchStatus::Ok ||
        line.pixels[0].rgb555 != UINT16_C(0x4567)) {
        return fail("Engine B OAM/OBJ VRAM/OBJ palette selection failed");
    }

    /* Engine B affine matrices occupy the corresponding second 1 KiB OAM half. */
    if (!write_affine_matrix(
            memory, 1u, 2u,
            UINT16_C(0x0100), 0u, 0u, UINT16_C(0x0100)) ||
        !write_oam(
            memory, 1u, 0u,
            UINT16_C(0x0100),
            static_cast<uint16_t>(2u << 9u),
            UINT16_C(0x0000))) {
        return fail("Engine B affine matrix fixture setup failed");
    }
    result = fetcher.render_scanline(1u, display, 0u, line);
    if (result.status != Nds2dObjFetchStatus::Ok ||
        line.pixels[0].rgb555 != UINT16_C(0x4567) ||
        line.pixels[7].rgb555 != UINT16_C(0x4567)) {
        return fail("Engine B affine matrix lookup or identity sampling failed");
    }

    std::puts("NDS Engine A/B OBJ OAM, affine identity/scale/rotation/double-size, 4bpp/8bpp, flips, priority and OBJ-window tests passed");
    return 0;
}
