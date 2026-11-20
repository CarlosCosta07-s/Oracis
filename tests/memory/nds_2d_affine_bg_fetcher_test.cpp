#include "oracis/memory/nds_2d_affine_bg_fetcher.hpp"
#include "oracis/memory/nds_2d_bg_compositor.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_2d_affine_bg_fetcher_test: %s\n", message);
    return 1;
}

bool write8(
    oracis::memory::GuestMemory &memory, uint32_t address, uint8_t value)
{
    return memory.write(NDS_ARM9, address, 1u, value) ==
        oracis::memory::Status::Ok;
}

bool write16(
    oracis::memory::GuestMemory &memory, uint32_t address, uint16_t value)
{
    return memory.write(NDS_ARM9, address, 2u, value) ==
        oracis::memory::Status::Ok;
}

bool set_affine(
    oracis::memory::NdsDisplayControl &display,
    uint32_t base,
    uint16_t pa,
    uint16_t pb,
    uint16_t pc,
    uint16_t pd,
    uint32_t reference_x,
    uint32_t reference_y)
{
    using oracis::memory::Status;
    return display.write(NDS_ARM9, base, 2u, pa) == Status::Ok &&
        display.write(NDS_ARM9, base + 2u, 2u, pb) == Status::Ok &&
        display.write(NDS_ARM9, base + 4u, 2u, pc) == Status::Ok &&
        display.write(NDS_ARM9, base + 6u, 2u, pd) == Status::Ok &&
        display.write(NDS_ARM9, base + 8u, 4u, reference_x) == Status::Ok &&
        display.write(NDS_ARM9, base + 12u, 4u, reference_y) == Status::Ok;
}

} // namespace

int main()
{
    using namespace oracis::memory;

    GuestMemory memory;
    if (memory.map_ram(
            UINT32_C(0x05000000), UINT32_C(0x00000800),
            static_cast<uint8_t>(Readable | Writable),
            "affine BG palette RAM", CpuAccessArm9) != Status::Ok ||
        memory.map_ram(
            UINT32_C(0x06000000), UINT32_C(0x00020000),
            static_cast<uint8_t>(Readable | Writable),
            "affine main BG VRAM", CpuAccessArm9) != Status::Ok ||
        memory.map_ram(
            UINT32_C(0x06200000), UINT32_C(0x00020000),
            static_cast<uint8_t>(Readable | Writable),
            "affine sub BG VRAM", CpuAccessArm9) != Status::Ok) {
        return fail("affine BG memory setup failed");
    }

    NdsInterruptControl interrupts(memory);
    NdsDisplayControl display(interrupts);
    if (display.install() != Status::Ok) {
        return fail("display registers failed to install");
    }

    /* Main Engine BG2 in Mode 2; 16x16 affine map, character block 1. */
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            UINT32_C(2) | (UINT32_C(1) << 10u)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 12u, 2u,
            UINT16_C(0x0004)) != Status::Ok ||
        !write8(memory, UINT32_C(0x06000000), 1u) ||
        !write8(memory, UINT32_C(0x0600000F), 1u) ||
        !write8(memory, UINT32_C(0x06004040), 1u) ||
        !write8(memory, UINT32_C(0x06004041), 2u) ||
        !write8(memory, UINT32_C(0x06004042), 3u) ||
        !write8(memory, UINT32_C(0x06004043), 4u) ||
        !write8(memory, UINT32_C(0x06004046), 2u) ||
        !write8(memory, UINT32_C(0x06004047), 5u) ||
        !write8(memory, UINT32_C(0x06004049), 2u) ||
        !write8(memory, UINT32_C(0x0600404A), 4u) ||
        !write8(memory, UINT32_C(0x0600404F), 3u) ||
        !write16(memory, UINT32_C(0x05000002), UINT16_C(0x001F)) ||
        !write16(memory, UINT32_C(0x05000004), UINT16_C(0x03E0)) ||
        !write16(memory, UINT32_C(0x05000006), UINT16_C(0x7C00)) ||
        !write16(memory, UINT32_C(0x05000008), UINT16_C(0x7FFF)) ||
        !write16(memory, UINT32_C(0x0500000A), UINT16_C(0x7C1F)) ||
        !set_affine(
            display, NdsDisplayControl::EngineABase + 0x20u,
            UINT16_C(0x0100), 0u, 0u, UINT16_C(0x0100), 0u, 0u)) {
        return fail("Engine A BG2 affine identity fixture setup failed");
    }

    Nds2dAffineBgFetcher fetcher(memory);
    std::array<Nds2dBgPixel, 256> line{};
    auto result = fetcher.render_scanline(0u, 2u, display, 0u, line);
    if (result.status != Nds2dBgFetchStatus::Ok ||
        line[0].transparent || line[0].rgb555 != UINT16_C(0x001F) ||
        line[1].rgb555 != UINT16_C(0x03E0) ||
        line[2].rgb555 != UINT16_C(0x7C00) ||
        line[3].rgb555 != UINT16_C(0x7FFF) ||
        line[7].rgb555 != UINT16_C(0x7C1F) ||
        !line[8].transparent ||
        line[0].priority != 0u || line[0].bg_index != 2u) {
        return fail("affine 8bpp identity map/tile/palette fetch failed");
    }

    /*
     * Signed PB=-1 and PC=+1 rotate the source lookup around reference (7,0):
     * line 0 maps screen (0,0)->source (7,0), (1,0)->(7,1). The negative
     * PB then moves line 1's origin left by one source pixel.
     */
    if (!set_affine(
            display, NdsDisplayControl::EngineABase + 0x20u,
            0u, UINT16_C(0xFF00), UINT16_C(0x0100), 0u,
            UINT32_C(0x00000700), 0u)) {
        return fail("signed affine rotation matrix setup failed");
    }
    display.reset_timing();
    result = fetcher.render_scanline(0u, 2u, display, 0u, line);
    if (result.status != Nds2dBgFetchStatus::Ok ||
        line[0].rgb555 != UINT16_C(0x7C1F) ||
        line[1].rgb555 != UINT16_C(0x7C00)) {
        return fail("signed PB/PC affine rotation mapping failed");
    }
    display.advance_bus_cycles(NdsDisplayControl::CyclesPerScanline);
    result = fetcher.render_scanline(0u, 2u, display, 1u, line);
    if (result.status != Nds2dBgFetchStatus::Ok ||
        line[0].rgb555 != UINT16_C(0x03E0)) {
        return fail("negative PB affine line accumulator failed");
    }

    /* PA=0.5 samples pairs of adjacent screen pixels from each source pixel. */
    if (!set_affine(
            display, NdsDisplayControl::EngineABase + 0x20u,
            UINT16_C(0x0080), 0u, 0u, UINT16_C(0x0100), 0u, 0u)) {
        return fail("affine horizontal scale register setup failed");
    }
    display.reset_timing();
    result = fetcher.render_scanline(0u, 2u, display, 0u, line);
    if (result.status != Nds2dBgFetchStatus::Ok ||
        line[0].rgb555 != UINT16_C(0x001F) ||
        line[1].rgb555 != UINT16_C(0x001F) ||
        line[2].rgb555 != UINT16_C(0x03E0) ||
        line[3].rgb555 != UINT16_C(0x03E0)) {
        return fail("affine horizontal scaling source coordinates failed");
    }

    /*
     * PB/PD advance the internal reference by one source pixel per scanline.
     * At line 1, x=0 maps to source (1,1), whose test texel is palette 2.
     */
    if (!set_affine(
            display, NdsDisplayControl::EngineABase + 0x20u,
            UINT16_C(0x0100), UINT16_C(0x0100),
            0u, UINT16_C(0x0100), 0u, 0u)) {
        return fail("affine per-scanline accumulator fixture setup failed");
    }
    display.reset_timing();
    result = fetcher.render_scanline(0u, 2u, display, 0u, line);
    if (result.status != Nds2dBgFetchStatus::Ok ||
        line[0].rgb555 != UINT16_C(0x001F) ||
        display.current_scanline() != 0u) {
        return fail("affine reference origin for scanline zero failed");
    }
    display.advance_bus_cycles(NdsDisplayControl::CyclesPerScanline);
    result = fetcher.render_scanline(0u, 2u, display, 1u, line);
    if (result.status != Nds2dBgFetchStatus::Ok ||
        display.current_scanline() != 1u ||
        line[0].rgb555 != UINT16_C(0x03E0)) {
        return fail("BG affine PB/PD reference accumulator failed");
    }

    /* A visible BG2X write immediately replaces the current line reference. */
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 0x28u, 4u,
            UINT32_C(0x00000200)) != Status::Ok) {
        return fail("mid-frame BG2X write failed");
    }
    result = fetcher.render_scanline(0u, 2u, display, 1u, line);
    if (result.status != Nds2dBgFetchStatus::Ok ||
        line[0].rgb555 != UINT16_C(0x7FFF)) {
        return fail("mid-frame affine reference write was not applied immediately");
    }

    /* Negative X is transparent without overflow, and wraps to map pixel 127 with it. */
    if (!set_affine(
            display, NdsDisplayControl::EngineABase + 0x20u,
            0u, 0u, 0u, 0u, UINT32_C(0x0FFFFF00), 0u) ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 12u, 2u,
            UINT16_C(0x0004)) != Status::Ok) {
        return fail("affine negative reference fixture setup failed");
    }
    display.reset_timing();
    result = fetcher.render_scanline(0u, 2u, display, 0u, line);
    if (result.status != Nds2dBgFetchStatus::Ok || !line[0].transparent) {
        return fail("affine out-of-range coordinate did not become transparent");
    }
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 12u, 2u,
            UINT16_C(0x2004)) != Status::Ok) {
        return fail("affine wraparound flag setup failed");
    }
    display.reset_timing();
    result = fetcher.render_scanline(0u, 2u, display, 0u, line);
    if (result.status != Nds2dBgFetchStatus::Ok ||
        line[0].transparent || line[0].rgb555 != UINT16_C(0x7C1F)) {
        return fail("affine wraparound did not fetch the wrapped edge texel");
    }

    /* BG mosaic rounds screen coordinates before the affine transform. */
    if (!set_affine(
            display, NdsDisplayControl::EngineABase + 0x20u,
            UINT16_C(0x0100), 0u, 0u, UINT16_C(0x0100), 0u, 0u) ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 12u, 2u,
            UINT16_C(0x0044)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 0x4Cu, 2u,
            UINT16_C(0x0001)) != Status::Ok) {
        return fail("affine mosaic fixture setup failed");
    }
    display.reset_timing();
    result = fetcher.render_scanline(0u, 2u, display, 0u, line);
    if (result.status != Nds2dBgFetchStatus::Ok ||
        line[0].rgb555 != line[1].rgb555 ||
        line[2].rgb555 != line[3].rgb555) {
        return fail("affine BG mosaic screen-coordinate sampling failed");
    }

    /* Sub Engine BG3 in Mode 1 uses its own BG VRAM and palette windows. */
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineBBase, 4u,
            UINT32_C(1) | (UINT32_C(1) << 11u)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineBBase + 14u, 2u,
            UINT16_C(0x0004)) != Status::Ok ||
        !write8(memory, UINT32_C(0x06200000), 1u) ||
        !write8(memory, UINT32_C(0x06204040), 6u) ||
        !write16(memory, UINT32_C(0x0500040C), UINT16_C(0x4567)) ||
        !set_affine(
            display, NdsDisplayControl::EngineBBase + 0x30u,
            UINT16_C(0x0100), 0u, 0u, UINT16_C(0x0100), 0u, 0u)) {
        return fail("Engine B BG3 affine fixture setup failed");
    }
    display.reset_timing();
    result = fetcher.render_scanline(1u, 3u, display, 0u, line);
    if (result.status != Nds2dBgFetchStatus::Ok ||
        line[0].transparent || line[0].rgb555 != UINT16_C(0x4567)) {
        return fail("Engine B BG3 affine VRAM/palette fetch failed");
    }

    /* The compositor dispatches BG2 affine alongside its normal BG/OBJ path. */
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            UINT32_C(2) | (UINT32_C(1) << 10u)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 12u, 2u,
            UINT16_C(0x0004)) != Status::Ok ||
        !set_affine(
            display, NdsDisplayControl::EngineABase + 0x20u,
            UINT16_C(0x0100), 0u, 0u, UINT16_C(0x0100), 0u, 0u) ||
        !write16(memory, UINT32_C(0x05000000), 0u) ||
        !write8(memory, UINT32_C(0x06000000), 1u)) {
        return fail("affine compositor integration fixture setup failed");
    }
    display.reset_timing();
    Nds2dBgCompositor compositor(memory);
    std::array<Nds2dCompositedPixel, 256> composed{};
    const Nds2dComposeResult compose_result =
        compositor.render_scanline(0u, display, 0u, composed);
    if (compose_result.status != Nds2dComposeStatus::Ok ||
        composed[0].top_layer != Nds2dLayer::Bg2 ||
        composed[0].rgb555 != UINT16_C(0x001F)) {
        return fail("BG affine fetch did not enter the BG/OBJ compositor");
    }

    /* The text fetcher must not reinterpret classic affine BG2 as tiled text. */
    Nds2dTextBgFetcher text_fetcher(memory);
    line[0].rgb555 = UINT16_C(0x4321);
    const auto text_on_affine = text_fetcher.render_scanline(
        0u, 2u, display, 0u, line);
    if (text_on_affine.status != Nds2dBgFetchStatus::UnsupportedMode ||
        line[0].rgb555 != UINT16_C(0x4321)) {
        return fail("text fetcher accepted an affine BG or modified output");
    }

    if (Nds2dAffineBgFetcher::is_affine_tile_bg(
            0u, 2u, UINT32_C(0)) ||
        Nds2dAffineBgFetcher::is_affine_tile_bg(
            0u, 2u, UINT32_C(1)) ||
        !Nds2dAffineBgFetcher::is_affine_tile_bg(
            0u, 2u, UINT32_C(2)) ||
        !Nds2dAffineBgFetcher::is_affine_tile_bg(
            0u, 2u, UINT32_C(4)) ||
        Nds2dAffineBgFetcher::is_affine_tile_bg(
            0u, 3u, UINT32_C(4)) ||
        !Nds2dAffineBgFetcher::is_affine_tile_bg(
            1u, 3u, UINT32_C(1))) {
        return fail("classic affine BG mode/layer selection table was incorrect");
    }

    if (!Nds2dAffineBgFetcher::is_extended_affine_tile_bg(
            0u, 3u, UINT32_C(3)) ||
        !Nds2dAffineBgFetcher::is_extended_affine_tile_bg(
            0u, 3u, UINT32_C(4)) ||
        !Nds2dAffineBgFetcher::is_extended_affine_tile_bg(
            0u, 2u, UINT32_C(5)) ||
        !Nds2dAffineBgFetcher::is_extended_affine_tile_bg(
            0u, 3u, UINT32_C(5)) ||
        Nds2dAffineBgFetcher::is_extended_affine_tile_bg(
            0u, 2u, UINT32_C(3)) ||
        Nds2dAffineBgFetcher::is_extended_affine_tile_bg(
            0u, 2u, UINT32_C(4)) ||
        Nds2dAffineBgFetcher::is_extended_affine_tile_bg(
            1u, 2u, UINT32_C(2))) {
        return fail("extended affine BG mode/layer selection table was incorrect");
    }

    /*
     * Mode 3 BG3 uses the extended 16-bit affine map. Tile 257 proves that
     * the ten-bit tile number is used; horizontal and vertical map flips
     * must mirror the tile-local source coordinates.
     */
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            UINT32_C(3) | (UINT32_C(1) << 11u)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 14u, 2u, 0u) !=
            Status::Ok ||
        !write16(memory, UINT32_C(0x06000000), UINT16_C(0x0101)) ||
        !write8(memory, UINT32_C(0x06004078), 3u) ||
        !write8(memory, UINT32_C(0x06004079), 4u) ||
        !set_affine(
            display, NdsDisplayControl::EngineABase + 0x30u,
            UINT16_C(0x0100), 0u, 0u, UINT16_C(0x0100), 0u, 0u)) {
        return fail("extended affine BG3 fixture setup failed");
    }
    display.reset_timing();
    result = fetcher.render_scanline(0u, 3u, display, 0u, line);
    if (result.status != Nds2dBgFetchStatus::Ok ||
        line[0].rgb555 != UINT16_C(0x001F) ||
        line[1].rgb555 != UINT16_C(0x03E0)) {
        return fail("extended affine 16-bit map or ten-bit tile index failed");
    }

    if (!write16(
            memory, UINT32_C(0x06000000), UINT16_C(0x0501))) {
        return fail("extended affine horizontal flip map write failed");
    }
    result = fetcher.render_scanline(0u, 3u, display, 0u, line);
    if (result.status != Nds2dBgFetchStatus::Ok ||
        line[0].rgb555 != UINT16_C(0x7C1F) ||
        line[1].rgb555 != UINT16_C(0x03E0)) {
        return fail("extended affine horizontal tile flip failed");
    }

    if (!write16(
            memory, UINT32_C(0x06000000), UINT16_C(0x0901))) {
        return fail("extended affine vertical flip map write failed");
    }
    result = fetcher.render_scanline(0u, 3u, display, 0u, line);
    if (result.status != Nds2dBgFetchStatus::Ok ||
        line[0].rgb555 != UINT16_C(0x7C00) ||
        line[1].rgb555 != UINT16_C(0x7FFF)) {
        return fail("extended affine vertical tile flip failed");
    }

    /* Bitmap and extended-palette variants fail closed without partial output. */
    line[0].rgb555 = UINT16_C(0x2345);
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 14u, 2u,
            UINT16_C(0x0080)) != Status::Ok) {
        return fail("extended BG bitmap mode setup failed");
    }
    result = fetcher.render_scanline(0u, 3u, display, 0u, line);
    if (result.status != Nds2dBgFetchStatus::UnsupportedMode ||
        line[0].rgb555 != UINT16_C(0x2345)) {
        return fail("extended BG bitmap mode did not fail transactionally");
    }
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 14u, 2u, 0u) !=
            Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            UINT32_C(3) | (UINT32_C(1) << 11u) |
                (UINT32_C(1) << 30u)) != Status::Ok) {
        return fail("extended BG palette mode setup failed");
    }
    result = fetcher.render_scanline(0u, 3u, display, 0u, line);
    if (result.status != Nds2dBgFetchStatus::UnsupportedMode ||
        line[0].rgb555 != UINT16_C(0x2345)) {
        return fail("extended BG palette mode did not fail transactionally");
    }

    /* The compositor must route Mode 3 BG3 into the extended tile fetcher. */
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            UINT32_C(3) | (UINT32_C(1) << 11u)) != Status::Ok ||
        !write16(
            memory, UINT32_C(0x06000000), UINT16_C(0x0101)) ||
        !set_affine(
            display, NdsDisplayControl::EngineABase + 0x30u,
            UINT16_C(0x0100), 0u, 0u, UINT16_C(0x0100), 0u, 0u)) {
        return fail("extended affine compositor fixture setup failed");
    }
    display.reset_timing();
    const Nds2dComposeResult extended_compose =
        compositor.render_scanline(0u, display, 0u, composed);
    if (extended_compose.status != Nds2dComposeStatus::Ok ||
        composed[0].top_layer != Nds2dLayer::Bg3 ||
        composed[0].rgb555 != UINT16_C(0x001F)) {
        return fail("extended affine BG3 did not reach the compositor");
    }

    std::puts("NDS classic and extended affine BG tile maps, flip attributes, matrix, accumulators, overflow, mosaic and compositor passed");
    return 0;
}
