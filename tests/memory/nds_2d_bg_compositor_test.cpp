#include "oracis/memory/nds_2d_bg_compositor.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_2d_bg_compositor_test: %s\n", message);
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

} // namespace

int main()
{
    using namespace oracis::memory;

    GuestMemory memory;
    if (memory.map_ram(
            UINT32_C(0x05000000), UINT32_C(0x00000800),
            static_cast<uint8_t>(Readable | Writable),
            "compositor palette RAM", CpuAccessArm9) != Status::Ok ||
        memory.map_ram(
            UINT32_C(0x06000000), UINT32_C(0x00020000),
            static_cast<uint8_t>(Readable | Writable),
            "compositor Engine A BG VRAM", CpuAccessArm9) != Status::Ok ||
        memory.map_ram(
            UINT32_C(0x06400000), UINT32_C(0x00040000),
            static_cast<uint8_t>(Readable | Writable),
            "compositor Main OBJ VRAM", CpuAccessArm9) != Status::Ok ||
        memory.map_ram(
            UINT32_C(0x07000000), UINT32_C(0x00000800),
            static_cast<uint8_t>(Readable | Writable),
            "compositor OAM", CpuAccessArm9) != Status::Ok) {
        return fail("synthetic compositor memory map setup failed");
    }

    /*
     * OAM resets are not assumed to disable entries. Mark every synthetic
     * slot disabled, then enable only the sprites each fixture declares.
     */
    for (unsigned object = 0u; object < 128u; ++object) {
        if (!write16(
                memory,
                UINT32_C(0x07000000) + object * UINT32_C(8),
                UINT16_C(0x0200))) {
            return fail("OAM disabled-entry fixture initialization failed");
        }
    }

    NdsInterruptControl interrupts(memory);
    NdsDisplayControl display(interrupts);
    if (display.install() != Status::Ok) {
        return fail("display register device failed to install");
    }

    /* Backdrop=blue, BG0 palette entry 1=red, BG1 bank 2 entry 1=green. */
    if (!write16(memory, UINT32_C(0x05000000), UINT16_C(0x7C00)) ||
        !write16(memory, UINT32_C(0x05000002), UINT16_C(0x001F)) ||
        !write16(memory, UINT32_C(0x05000042), UINT16_C(0x03E0))) {
        return fail("backdrop/BG palette fixture setup failed");
    }

    /* Screen entries point at tile 1; each tile's first pixels use color 1. */
    if (!write16(memory, UINT32_C(0x06000800), UINT16_C(0x0001)) ||
        !write16(memory, UINT32_C(0x06001000), UINT16_C(0x2001)) ||
        !write32(memory, UINT32_C(0x06004020), UINT32_C(0x11111111)) ||
        !write32(memory, UINT32_C(0x06008020), UINT32_C(0x11111111))) {
        return fail("BG map/tile fixture setup failed");
    }

    /* BG0: priority 1, character base 1, screen base 1. */
    /* BG1: priority 0, character base 2, screen base 2. */
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            (UINT32_C(1) << 8u) | (UINT32_C(1) << 9u)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 8u, 2u,
            UINT16_C(0x0105)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 10u, 2u,
            UINT16_C(0x0208)) != Status::Ok) {
        return fail("BG0/BG1 enable and control setup failed");
    }


    /* OBJ palette index 1 and the first visible sprite tile. */
    if (!write16(memory, UINT32_C(0x05000202), UINT16_C(0x7C1F)) ||
        !write32(memory, UINT32_C(0x06400000), UINT32_C(0x11111111)) ||
        !write16(memory, UINT32_C(0x07000000), UINT16_C(0x0000)) ||
        !write16(memory, UINT32_C(0x07000002), UINT16_C(0x0000)) ||
        !write16(memory, UINT32_C(0x07000004), UINT16_C(0x0000))) {
        return fail("OBJ compositor fixture setup failed");
    }

    Nds2dBgCompositor compositor(memory);
    std::array<Nds2dCompositedPixel, 256> line{};
    auto result = compositor.render_scanline(0u, display, 0u, line);
    if (result.status != Nds2dComposeStatus::Ok ||
        line[0].rgb555 != UINT16_C(0x03E0) ||
        line[0].top_layer != Nds2dLayer::Bg1 ||
        line[0].second_layer != Nds2dLayer::Bg0 ||
        !line[0].second_pixel_available ||
        line[8].top_layer != Nds2dLayer::Backdrop ||
        line[8].rgb555 != UINT16_C(0x7C00)) {
        return fail("priority ordering, lower pixel selection or backdrop failed");
    }

    /* At equal BG priority, OBJ is above BG; the lower candidate is retained. */
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            (UINT32_C(1) << 8u) | (UINT32_C(1) << 9u) |
                (UINT32_C(1) << 12u)) != Status::Ok) {
        return fail("OBJ compositor enable failed");
    }
    result = compositor.render_scanline(0u, display, 0u, line);
    if (result.status != Nds2dComposeStatus::Ok ||
        line[0].top_layer != Nds2dLayer::Object ||
        line[0].top_object_index != 0u ||
        line[0].second_layer != Nds2dLayer::Bg1) {
        return fail("OBJ/BG priority merge failed");
    }

    /* Semi-transparent OBJ forces alpha and blends against BG1 target 2. */
    if (!write16(memory, UINT32_C(0x07000000), UINT16_C(0x0400)) ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 0x50u, 2u,
            UINT16_C(0x0240)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 0x52u, 2u,
            UINT16_C(0x0808)) != Status::Ok) {
        return fail("semi-transparent OBJ fixture setup failed");
    }
    result = compositor.render_scanline(0u, display, 0u, line);
    if (result.status != Nds2dComposeStatus::Ok ||
        line[0].top_layer != Nds2dLayer::Object ||
        !line[0].top_object_semi_transparent ||
        line[0].second_layer != Nds2dLayer::Bg1 ||
        line[0].rgb555 != UINT16_C(0x4210)) {
        return fail("semi-transparent OBJ first-target alpha blend failed");
    }

    /*
     * A semitransparent top OBJ blends against the next OBJ candidate when
     * OBJ is enabled as a second target; BG1 must not displace that OBJ layer.
     */
    if (!write16(memory, UINT32_C(0x05000202), UINT16_C(0x001F)) ||
        !write16(memory, UINT32_C(0x05000204), UINT16_C(0x03E0)) ||
        !write32(memory, UINT32_C(0x06400020), UINT32_C(0x22222222)) ||
        !write16(memory, UINT32_C(0x07000000), UINT16_C(0x0400)) ||
        !write16(memory, UINT32_C(0x07000002), UINT16_C(0x0000)) ||
        !write16(memory, UINT32_C(0x07000004), UINT16_C(0x0000)) ||
        !write16(memory, UINT32_C(0x07000008), UINT16_C(0x0000)) ||
        !write16(memory, UINT32_C(0x0700000A), UINT16_C(0x0000)) ||
        !write16(memory, UINT32_C(0x0700000C), UINT16_C(0x0001)) ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            (UINT32_C(1) << 8u) | (UINT32_C(1) << 9u) |
                (UINT32_C(1) << 12u)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 0x50u, 2u,
            UINT16_C(0x1050)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 0x52u, 2u,
            UINT16_C(0x0808)) != Status::Ok) {
        return fail("OBJ-over-OBJ alpha fixture setup failed");
    }
    result = compositor.render_scanline(0u, display, 0u, line);
    if (result.status != Nds2dComposeStatus::Ok ||
        line[0].top_layer != Nds2dLayer::Object ||
        line[0].top_object_index != 0u ||
        !line[0].top_object_semi_transparent ||
        line[0].second_layer != Nds2dLayer::Backdrop ||
        line[0].second_pixel_available ||
        line[0].second_object_index != UINT8_C(0xFF) ||
        line[0].rgb555 != UINT16_C(0x001F)) {
        return fail("a second OBJ was incorrectly used as an alpha blend target");
    }

    /*
     * Bitmap OBJ uses alphaOAM=7 (EVA=8) and ignores BLDALPHA. A normal OBJ
     * lies between it and BG1, but the bitmap must blend with the selected
     * BG1 second target rather than with the intervening OBJ.
     */
    if (!write16(memory, UINT32_C(0x06400000), UINT16_C(0x801F)) ||
        !write16(memory, UINT32_C(0x07000000), UINT16_C(0x0C00)) ||
        !write16(memory, UINT32_C(0x07000002), UINT16_C(0x0000)) ||
        !write16(memory, UINT32_C(0x07000004), UINT16_C(0x7000)) ||
        !write16(memory, UINT32_C(0x07000008), UINT16_C(0x0000)) ||
        !write16(memory, UINT32_C(0x0700000A), UINT16_C(0x0000)) ||
        !write16(memory, UINT32_C(0x0700000C), UINT16_C(0x0001)) ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            (UINT32_C(1) << 8u) | (UINT32_C(1) << 9u) |
                (UINT32_C(1) << 12u)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 0x50u, 2u,
            UINT16_C(0x0210)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 0x52u, 2u,
            0u) != Status::Ok) {
        return fail("bitmap OBJ blend fixture setup failed");
    }
    result = compositor.render_scanline(0u, display, 0u, line);
    if (result.status != Nds2dComposeStatus::Ok ||
        line[0].top_layer != Nds2dLayer::Object ||
        !line[0].top_object_bitmap ||
        line[0].top_object_bitmap_alpha_eva != 8u ||
        line[0].top_object_semi_transparent ||
        line[0].second_layer != Nds2dLayer::Bg1 ||
        !line[0].second_pixel_available ||
        line[0].second_object_index != UINT8_C(0xFF) ||
        line[0].rgb555 != UINT16_C(0x0210)) {
        return fail("bitmap OBJ alpha or BG second-target composition failed");
    }

    /* OBJ-window coverage changes the per-pixel window region and layer mask. */
    if (!write16(memory, UINT32_C(0x07000000), UINT16_C(0x0800)) ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            (UINT32_C(1) << 8u) | (UINT32_C(1) << 9u) |
                (UINT32_C(1) << 15u)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 0x4Au, 2u,
            UINT16_C(0x2100)) != Status::Ok) {
        return fail("OBJ-window compositor fixture setup failed");
    }
    result = compositor.render_scanline(0u, display, 0u, line);
    if (result.status != Nds2dComposeStatus::Ok ||
        line[0].window_region != Nds2dWindowRegion::ObjectWindow ||
        line[0].top_layer != Nds2dLayer::Bg0 ||
        line[8].window_region != Nds2dWindowRegion::Outside ||
        line[8].top_layer != Nds2dLayer::Backdrop) {
        return fail("OBJ-window region/layer mask composition failed");
    }

    /* Turn OBJ/OBJ-window off before the BG-only ordering checks. */
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            (UINT32_C(1) << 8u) | (UINT32_C(1) << 9u)) != Status::Ok) {
        return fail("OBJ disable after compositor tests failed");
    }

    /* Equal priorities are resolved BG0, BG1, BG2, BG3. */
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 8u, 2u,
            UINT16_C(0x0104)) != Status::Ok) {
        return fail("equal-priority fixture update failed");
    }
    result = compositor.render_scanline(0u, display, 0u, line);
    if (result.status != Nds2dComposeStatus::Ok ||
        line[0].top_layer != Nds2dLayer::Bg0 ||
        line[0].rgb555 != UINT16_C(0x001F) ||
        line[0].second_layer != Nds2dLayer::Bg1) {
        return fail("equal-priority BG index tie-break failed");
    }

    /* WIN0 allows BG0 only within x=[0,2); outside it hides both BGs. */
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            (UINT32_C(1) << 8u) | (UINT32_C(1) << 9u) |
                (UINT32_C(1) << 13u)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 0x40u, 2u,
            UINT16_C(0x0002)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 0x44u, 2u,
            UINT16_C(0x00C0)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 0x48u, 2u,
            UINT16_C(0x0001)) != Status::Ok) {
        return fail("WIN0 fixture setup failed");
    }
    result = compositor.render_scanline(0u, display, 0u, line);
    if (result.status != Nds2dComposeStatus::Ok ||
        line[0].top_layer != Nds2dLayer::Bg0 ||
        line[0].window_region != Nds2dWindowRegion::Window0 ||
        line[2].top_layer != Nds2dLayer::Backdrop ||
        line[2].window_region != Nds2dWindowRegion::Outside) {
        return fail("window layer masking or backdrop escape failed");
    }

    /* Alpha blending uses the visible BG0 pixel and the next BG1 pixel. */
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            (UINT32_C(1) << 8u) | (UINT32_C(1) << 9u)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 0x50u, 2u,
            UINT16_C(0x0241)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 0x52u, 2u,
            UINT16_C(0x0808)) != Status::Ok) {
        return fail("alpha-blend fixture setup failed");
    }
    result = compositor.render_scanline(0u, display, 0u, line);
    if (result.status != Nds2dComposeStatus::Ok ||
        line[0].top_layer != Nds2dLayer::Bg0 ||
        line[0].second_layer != Nds2dLayer::Bg1 ||
        line[0].rgb555 != UINT16_C(0x0210)) {
        return fail("BG0/BG1 first/second-target alpha blend failed");
    }

    /* Extended tile-map palette slots remain unsupported and fail closed. */
    std::array<Nds2dCompositedPixel, 256> preserved = line;
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            (UINT32_C(3) << 0u) | (UINT32_C(1) << 11u) |
                (UINT32_C(1) << 30u)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 14u, 2u, 0u) !=
            Status::Ok) {
        return fail("unsupported extended tile palette fixture setup failed");
    }
    result = compositor.render_scanline(0u, display, 0u, line);
    if (result.status != Nds2dComposeStatus::UnsupportedBackgroundMode ||
        result.background_index != 3u ||
        line[0].rgb555 != preserved[0].rgb555 ||
        line[0].top_layer != preserved[0].top_layer) {
        return fail("unsupported extended tile palette did not fail transactionally");
    }

    /* Missing backdrop/palette memory also preserves the caller's scanline. */
    GuestMemory empty_memory;
    Nds2dBgCompositor unmapped_compositor(empty_memory);
    NdsInterruptControl empty_interrupts(empty_memory);
    NdsDisplayControl empty_display(empty_interrupts);
    if (empty_display.install() != Status::Ok ||
        empty_display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            UINT32_C(0)) != Status::Ok) {
        return fail("empty display setup failed");
    }
    auto untouched = preserved;
    result = unmapped_compositor.render_scanline(
        0u, empty_display, 0u, untouched);
    if (result.status != Nds2dComposeStatus::GuestMemoryError ||
        result.memory_status != Status::Unmapped ||
        untouched[0].rgb555 != preserved[0].rgb555) {
        return fail("missing backdrop did not fail transactionally");
    }

    /*
     * Text BG extended palettes must pass through the same compositor
     * while keeping palette VRAM inaccessible to both guest CPUs.
     */
    GuestMemory extended_text_memory;
    if (extended_text_memory.map_ram(
            UINT32_C(0x05000000), UINT32_C(0x800),
            static_cast<uint8_t>(Readable | Writable),
            "extended text standard palette", CpuAccessArm9) != Status::Ok ||
        extended_text_memory.map_ram(
            UINT32_C(0x06000000), UINT32_C(0x20000),
            static_cast<uint8_t>(Readable | Writable),
            "extended text BG VRAM", CpuAccessArm9) != Status::Ok) {
        return fail("compositor text extended palette memory setup failed");
    }
    NdsInterruptControl extended_text_interrupts(extended_text_memory);
    NdsDisplayControl extended_text_display(extended_text_interrupts);
    NdsVramControl extended_text_vram(extended_text_memory);
    if (extended_text_display.install() != Status::Ok ||
        extended_text_vram.install() != Status::Ok ||
        extended_text_vram.write(
            NDS_ARM9, NdsVramControl::VramCntEAddress, 1u,
            UINT32_C(0x80)) != Status::Ok ||
        extended_text_memory.write(
            NDS_ARM9,
            UINT32_C(0x06880000) + UINT32_C(0x0000) +
                UINT32_C(0x0200) + UINT32_C(2),
            2u, UINT32_C(0x4A2B)) != Status::Ok ||
        extended_text_vram.write(
            NDS_ARM9, NdsVramControl::VramCntEAddress, 1u,
            UINT32_C(0x84)) != Status::Ok ||
        extended_text_display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            (UINT32_C(1) << 8u) | (UINT32_C(1) << 30u)) != Status::Ok ||
        extended_text_display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 8u, 2u,
            UINT16_C(0x0084)) != Status::Ok ||
        extended_text_display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 10u, 2u,
            UINT16_C(0x0000)) != Status::Ok ||
        !write16(
            extended_text_memory, UINT32_C(0x06000000), UINT16_C(0x1001)) ||
        !write32(
            extended_text_memory, UINT32_C(0x06004040), UINT32_C(0x00000001))) {
        return fail("compositor text extended palette fixture initialization failed");
    }
    Nds2dBgCompositor extended_text_compositor(
        extended_text_memory, &extended_text_vram);
    std::array<Nds2dCompositedPixel, 256> extended_text_line{};
    const Nds2dComposeResult extended_text_result =
        extended_text_compositor.render_scanline(
            0u, extended_text_display, 0u, extended_text_line);
    if (extended_text_result.status != Nds2dComposeStatus::Ok ||
        extended_text_line[0].top_layer != Nds2dLayer::Bg0 ||
        extended_text_line[0].rgb555 != UINT16_C(0x4A2B) ||
        extended_text_memory.read(
            NDS_ARM9, UINT32_C(0x06880000), 2u).status == Status::Ok) {
        return fail("text extended palette was not composed or leaked to CPU VRAM");
    }

    std::puts("NDS BG scanline priority, backdrop, window and alpha compositor passed");
    return 0;
}
