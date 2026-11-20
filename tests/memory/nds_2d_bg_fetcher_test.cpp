#include "oracis/memory/nds_2d_bg_fetcher.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_2d_bg_fetcher_test: %s\n", message);
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
            "test palette", CpuAccessArm9) != Status::Ok ||
        memory.map_ram(
            UINT32_C(0x06000000), UINT32_C(0x00020000),
            static_cast<uint8_t>(Readable | Writable),
            "test main BG VRAM", CpuAccessArm9) != Status::Ok ||
        memory.map_ram(
            UINT32_C(0x06200000), UINT32_C(0x00020000),
            static_cast<uint8_t>(Readable | Writable),
            "test sub BG VRAM", CpuAccessArm9) != Status::Ok) {
        return fail("synthetic BG memory map setup failed");
    }

    NdsInterruptControl interrupts(memory);
    NdsDisplayControl display(interrupts);
    if (display.install() != Status::Ok) {
        return fail("display register device failed to install");
    }
    Nds2dTextBgFetcher fetcher(memory);

    /* Engine A BG0: 4bpp, character base 16 KiB, screen base 0. */
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            UINT32_C(1) << 8u) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 8u, 2u,
            UINT16_C(0x0006)) != Status::Ok ||
        !write16(memory, UINT32_C(0x06000000), UINT16_C(0x2001)) ||
        !write32(memory, UINT32_C(0x06004020), UINT32_C(0x87654321)) ||
        !write16(memory, UINT32_C(0x05000042), UINT16_C(0x1234)) ||
        !write16(memory, UINT32_C(0x05000044), UINT16_C(0x2345)) ||
        !write16(memory, UINT32_C(0x05000046), UINT16_C(0x3456)) ||
        !write16(memory, UINT32_C(0x05000048), UINT16_C(0x4567)) ||
        !write16(memory, UINT32_C(0x0500004A), UINT16_C(0x5678)) ||
        !write16(memory, UINT32_C(0x0500004C), UINT16_C(0x6789)) ||
        !write16(memory, UINT32_C(0x0500004E), UINT16_C(0x789A)) ||
        !write16(memory, UINT32_C(0x05000050), UINT16_C(0x09AB))) {
        return fail("Engine A BG0 4bpp fixture setup failed");
    }

    std::array<Nds2dBgPixel, 256> scanline{};
    auto result = fetcher.render_scanline(0u, 0u, display, 0u, scanline);
    if (result.status != Nds2dBgFetchStatus::Ok ||
        scanline[0].rgb555 != UINT16_C(0x1234) ||
        scanline[1].rgb555 != UINT16_C(0x2345) ||
        scanline[2].rgb555 != UINT16_C(0x3456) ||
        scanline[3].rgb555 != UINT16_C(0x4567) ||
        scanline[7].rgb555 != UINT16_C(0x09AB) ||
        scanline[0].transparent || scanline[7].transparent ||
        !scanline[8].transparent ||
        scanline[0].priority != 2u || scanline[0].bg_index != 0u) {
        return fail("Engine A 4bpp tile/map/palette fetch failed");
    }

    /* Horizontal flip reverses the tile's eight source pixels. */
    if (!write16(memory, UINT32_C(0x06000000), UINT16_C(0x2401))) {
        return fail("horizontal flip map entry write failed");
    }
    result = fetcher.render_scanline(0u, 0u, display, 0u, scanline);
    if (result.status != Nds2dBgFetchStatus::Ok ||
        scanline[0].rgb555 != UINT16_C(0x09AB) ||
        scanline[1].rgb555 != UINT16_C(0x789A)) {
        return fail("4bpp horizontal flip did not reverse tile sampling");
    }

    /* BG mosaic samples the upper-left screen pixel of each mosaic block. */
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 8u, 2u,
            UINT16_C(0x0046)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 0x4Cu, 2u,
            UINT16_C(0x0001)) != Status::Ok ||
        !write16(memory, UINT32_C(0x06000000), UINT16_C(0x2001))) {
        return fail("mosaic BG0 fixture setup failed");
    }
    result = fetcher.render_scanline(0u, 0u, display, 0u, scanline);
    if (result.status != Nds2dBgFetchStatus::Ok ||
        scanline[0].rgb555 != scanline[1].rgb555 ||
        scanline[0].rgb555 != UINT16_C(0x1234)) {
        return fail("BG0 mosaic was not applied before tile lookup");
    }

    /* Engine B BG1: 8bpp character data and its separate Sub palette. */
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineBBase, 4u,
            UINT32_C(1) << 9u) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineBBase + 10u, 2u,
            UINT16_C(0x0088)) != Status::Ok ||
        !write16(memory, UINT32_C(0x06200000), UINT16_C(0x0002)) ||
        !write32(memory, UINT32_C(0x06208080), UINT32_C(0x00000605)) ||
        !write16(memory, UINT32_C(0x0500040A), UINT16_C(0x4567)) ||
        !write16(memory, UINT32_C(0x0500040C), UINT16_C(0x5678))) {
        return fail("Engine B BG1 8bpp fixture setup failed");
    }
    result = fetcher.render_scanline(1u, 1u, display, 0u, scanline);
    if (result.status != Nds2dBgFetchStatus::Ok ||
        scanline[0].rgb555 != UINT16_C(0x4567) ||
        scanline[1].rgb555 != UINT16_C(0x5678) ||
        !scanline[2].transparent ||
        scanline[0].priority != 0u ||
        scanline[0].bg_index != 1u) {
        return fail("Engine B 8bpp text BG fetch or Sub palette selection failed");
    }

    /* Unmapped source VRAM must fail without partially modifying the caller. */
    GuestMemory empty_memory;
    Nds2dTextBgFetcher unmapped_fetcher(empty_memory);
    std::array<Nds2dBgPixel, 256> preserved{};
    preserved[0].rgb555 = UINT16_C(0x4321);
    const auto failed = unmapped_fetcher.render_scanline(
        0u, 0u, display, 0u, preserved);
    if (failed.status != Nds2dBgFetchStatus::GuestMemoryError ||
        failed.memory_status != Status::Unmapped ||
        preserved[0].rgb555 != UINT16_C(0x4321)) {
        return fail("unmapped VRAM fetch did not fail transactionally");
    }

    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            UINT32_C(0x00000108)) != Status::Ok) {
        return fail("BG0 3D selection register write failed");
    }
    result = fetcher.render_scanline(0u, 0u, display, 0u, scanline);
    if (result.status != Nds2dBgFetchStatus::UnsupportedMode) {
        return fail("BG0 3D input was incorrectly fetched as a text BG");
    }

    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u, 0u) != Status::Ok) {
        return fail("BG0 disable register write failed");
    }
    result = fetcher.render_scanline(0u, 0u, display, 0u, scanline);
    if (result.status != Nds2dBgFetchStatus::Disabled ||
        !scanline[0].transparent || scanline[0].bg_index != 0u) {
        return fail("disabled BG did not yield a transparent scanline");
    }

    /*
     * DISPCNT.30 promotes text BGs with 16-bit maps to 8bpp and selects
     * the extended 256-color palette using map-entry bits 12-15.
     */
    NdsVramControl text_palette_vram(memory);
    if (text_palette_vram.install() != Status::Ok ||
        text_palette_vram.write(
            NDS_ARM9, NdsVramControl::VramCntEAddress, 1u,
            UINT32_C(0x80)) != Status::Ok ||
        memory.write(
            NDS_ARM9,
            UINT32_C(0x06880000) + UINT32_C(0x0000) +
                UINT32_C(0x0A00) + UINT32_C(20),
            2u, UINT32_C(0x1357)) != Status::Ok ||
        memory.write(
            NDS_ARM9,
            UINT32_C(0x06880000) + UINT32_C(0x4000) +
                UINT32_C(0x0A00) + UINT32_C(20),
            2u, UINT32_C(0x5A3C)) != Status::Ok ||
        memory.write(
            NDS_ARM9,
            UINT32_C(0x06880000) + UINT32_C(0x2000) +
                UINT32_C(0x0B00) + UINT32_C(22),
            2u, UINT32_C(0x2468)) != Status::Ok ||
        memory.write(
            NDS_ARM9,
            UINT32_C(0x06880000) + UINT32_C(0x6000) +
                UINT32_C(0x0B00) + UINT32_C(22),
            2u, UINT32_C(0x6B4D)) != Status::Ok ||
        text_palette_vram.write(
            NDS_ARM9, NdsVramControl::VramCntEAddress, 1u,
            UINT32_C(0x84)) != Status::Ok) {
        return fail("text extended palette VRAM E fixture setup failed");
    }

    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            (UINT32_C(1) << 8u) | (UINT32_C(1) << 30u)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 8u, 2u,
            UINT16_C(0x2004)) != Status::Ok ||
        !write16(memory, UINT32_C(0x06000000), UINT16_C(0xA001)) ||
        !write32(memory, UINT32_C(0x06004040), UINT32_C(0x0000000A))) {
        return fail("BG0 extended palette slot 2 fixture setup failed");
    }
    Nds2dTextBgFetcher extended_text_fetcher(
        memory, &text_palette_vram);
    result = extended_text_fetcher.render_scanline(
        0u, 0u, display, 0u, scanline);
    if (result.status != Nds2dBgFetchStatus::Ok ||
        scanline[0].transparent || scanline[0].rgb555 != UINT16_C(0x5A3C) ||
        !scanline[1].transparent) {
        return fail("BG0 extended palette slot 2 or promoted 8bpp fetch failed");
    }

    /* BG0CNT.13=0 routes the same BG to slot 0 instead of slot 2. */
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 8u, 2u,
            UINT16_C(0x0004)) != Status::Ok) {
        return fail("BG0 extended palette slot 0 selection setup failed");
    }
    result = extended_text_fetcher.render_scanline(
        0u, 0u, display, 0u, scanline);
    if (result.status != Nds2dBgFetchStatus::Ok ||
        scanline[0].transparent || scanline[0].rgb555 != UINT16_C(0x1357)) {
        return fail("BG0CNT.13 did not switch between slots 0 and 2");
    }

    /* BG1CNT.13 selects slot 3; with zero it selects slot 1. */
    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase, 4u,
            (UINT32_C(1) << 9u) | (UINT32_C(1) << 30u)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 10u, 2u,
            UINT16_C(0x2104)) != Status::Ok ||
        !write16(memory, UINT32_C(0x06000800), UINT16_C(0xB001)) ||
        !write32(memory, UINT32_C(0x06004040), UINT32_C(0x0000000B))) {
        return fail("BG1 extended palette slot 3 fixture setup failed");
    }
    result = extended_text_fetcher.render_scanline(
        0u, 1u, display, 0u, scanline);
    if (result.status != Nds2dBgFetchStatus::Ok ||
        scanline[0].transparent || scanline[0].rgb555 != UINT16_C(0x6B4D)) {
        return fail("BG1CNT.13 did not select extended palette slot 3");
    }

    if (display.write(
            NDS_ARM9, NdsDisplayControl::EngineABase + 10u, 2u,
            UINT16_C(0x0104)) != Status::Ok) {
        return fail("BG1 extended palette slot 1 selection setup failed");
    }
    result = extended_text_fetcher.render_scanline(
        0u, 1u, display, 0u, scanline);
    if (result.status != Nds2dBgFetchStatus::Ok ||
        scanline[0].transparent || scanline[0].rgb555 != UINT16_C(0x2468)) {
        return fail("BG1CNT.13 did not switch to extended palette slot 1");
    }

    /* Missing VRAM assignment is rejected without committing a partial line. */
    GuestMemory empty_vram_memory;
    NdsVramControl empty_vram(empty_vram_memory);
    if (empty_vram.install() != Status::Ok) {
        return fail("unallocated text extended palette VRAM install failed");
    }
    Nds2dTextBgFetcher no_palette_fetcher(memory, &empty_vram);
    scanline[0].rgb555 = UINT16_C(0x2345);
    result = no_palette_fetcher.render_scanline(
        0u, 1u, display, 0u, scanline);
    if (result.status != Nds2dBgFetchStatus::UnsupportedMode ||
        scanline[0].rgb555 != UINT16_C(0x2345)) {
        return fail("unallocated text extended palette did not fail closed");
    }

    std::puts("NDS Engine A/B text BG tile, map, palette, mosaic and transparency fetch passed");
    return 0;
}
