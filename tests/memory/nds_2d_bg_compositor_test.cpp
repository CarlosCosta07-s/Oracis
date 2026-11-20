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
            "compositor Engine A BG VRAM", CpuAccessArm9) != Status::Ok) {
        return fail("synthetic compositor memory map setup failed");
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

    /* A non-text BG mode must fail closed and preserve the prior output. */
    std::array<Nds2dCompositedPixel, 256> preserved = line;
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            (UINT32_C(1) << 0u) | (UINT32_C(1) << 11u)) != Status::Ok) {
        return fail("unsupported BG mode fixture setup failed");
    }
    result = compositor.render_scanline(0u, display, 0u, line);
    if (result.status != Nds2dComposeStatus::UnsupportedBackgroundMode ||
        result.background_index != 3u ||
        line[0].rgb555 != preserved[0].rgb555 ||
        line[0].top_layer != preserved[0].top_layer) {
        return fail("unsupported affine BG did not fail transactionally");
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

    std::puts("NDS BG scanline priority, backdrop, window and alpha compositor passed");
    return 0;
}
