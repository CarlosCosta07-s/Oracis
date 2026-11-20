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

    std::puts("NDS Engine A/B text BG tile, map, palette, mosaic and transparency fetch passed");
    return 0;
}
