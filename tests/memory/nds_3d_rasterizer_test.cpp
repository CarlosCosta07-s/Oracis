#include "oracis/memory/guest_memory.hpp"
#include "oracis/memory/nds_3d_rasterizer.hpp"
#include "oracis/memory/nds_3d_texture_sampler.hpp"
#include "oracis/memory/nds_geometry_engine.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"
#include "oracis/memory/nds_vram_control.hpp"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_3d_rasterizer_test: %s\n", message);
    return 1;
}

bool write_command(
    oracis::memory::NdsGeometryEngine &geometry,
    uint8_t command,
    uint32_t parameter = 0u)
{
    const uint32_t address =
        oracis::memory::NdsGeometryEngine::GxFifoBase +
        static_cast<uint32_t>(command) * 4u;
    return geometry.write(NDS_ARM9, address, 4u, parameter) ==
        oracis::memory::Status::Ok;
}

bool write_halfword(
    oracis::memory::GuestMemory &memory,
    uint32_t address,
    uint16_t value)
{
    return memory.write(NDS_ARM9, address, 2u, value) ==
        oracis::memory::Status::Ok;
}

struct Capture {
    uint32_t count = 0u;
    bool center_seen = false;
    oracis::memory::Nds3dFragment center{};
};

void capture_fragment(
    void *context,
    const oracis::memory::Nds3dFragment &fragment) noexcept
{
    auto *capture = static_cast<Capture *>(context);
    ++capture->count;
    if (fragment.x == 127u && fragment.y == 96u) {
        capture->center_seen = true;
        capture->center = fragment;
    }
}

uint32_t encode_vtx10(int32_t x, int32_t y, int32_t z) noexcept
{
    return (static_cast<uint32_t>(x) & UINT32_C(0x3FF)) |
        ((static_cast<uint32_t>(y) & UINT32_C(0x3FF)) << 10u) |
        ((static_cast<uint32_t>(z) & UINT32_C(0x3FF)) << 20u);
}

} // namespace

int main()
{
    using namespace oracis::memory;

    GuestMemory memory;
    NdsInterruptControl interrupts(memory);
    NdsGeometryEngine geometry(interrupts);
    NdsVramControl vram(memory);
    if (geometry.install() != Status::Ok ||
        vram.install() != Status::Ok) {
        return fail("geometry or VRAM device installation failed");
    }

    /* Prepare a direct-color texel in VRAM A, then assign A to texture slot 0. */
    if (vram.write(
            NDS_ARM9, NdsVramControl::VramCntAAddress, 1u,
            UINT32_C(0x80)) != Status::Ok ||
        !write_halfword(memory, UINT32_C(0x06800000), UINT16_C(0xFC00)) ||
        vram.write(
            NDS_ARM9, NdsVramControl::VramCntAAddress, 1u,
            UINT32_C(0x83)) != Status::Ok) {
        return fail("direct-color texture setup failed");
    }

    /* One front-facing triangle, white vertex color, direct-color texture. */
    if (!write_command(geometry, 0x29u,
            (UINT32_C(31) << 16u) | (UINT32_C(1) << 7u))) {
        return fail("POLYGON_ATTR enqueue failed");
    }
    geometry.advance_bus_cycles(1u);
    if (!write_command(geometry, 0x2Au, UINT32_C(7) << 26u)) {
        return fail("TEXIMAGE_PARAM enqueue failed");
    }
    geometry.advance_bus_cycles(1u);
    if (!write_command(geometry, 0x40u, 0u)) {
        return fail("BEGIN_VTXS enqueue failed");
    }
    geometry.advance_bus_cycles(1u);

    const uint32_t positions[3] = {
        encode_vtx10(-64, -64, 0),
        encode_vtx10(64, -64, 0),
        encode_vtx10(0, 64, 0)
    };
    for (uint32_t position : positions) {
        if (!write_command(geometry, 0x22u, 0u)) {
            return fail("TEXCOORD enqueue failed");
        }
        geometry.advance_bus_cycles(1u);
        if (!write_command(geometry, 0x20u, UINT32_C(0x7FFF))) {
            return fail("COLOR enqueue failed");
        }
        geometry.advance_bus_cycles(1u);
        if (!write_command(geometry, 0x24u, position)) {
            return fail("VTX_10 enqueue failed");
        }
        geometry.advance_bus_cycles(8u);
    }

    if (geometry.polygon_count() != 1u ||
        !write_command(geometry, 0x50u, 0u)) {
        return fail("triangle was not assembled or SWAP_BUFFERS enqueue failed");
    }
    geometry.advance_bus_cycles(1u);
    if (!geometry.swap_waiting_for_vblank()) {
        return fail("SWAP_BUFFERS did not wait for VBlank");
    }
    geometry.notify_vblank();
    geometry.advance_bus_cycles(392u);
    if (geometry.render_polygon_count() != 1u ||
        geometry.render_vertex_count() != 3u) {
        return fail("SWAP_BUFFERS did not publish the render buffer");
    }

    Nds3dTextureSampler textures(vram);
    Nds3dRasterizer rasterizer(geometry, textures);
    Capture capture{};
    if (rasterizer.render_scanline(
            96u, capture_fragment, &capture) != Nds3dRasterStatus::Ok ||
        capture.count == 0u || !capture.center_seen) {
        return fail("scanline rasterizer did not emit the expected center fragment");
    }

    if (!capture.center.textured ||
        capture.center.red6 != 0u ||
        capture.center.green6 != 0u ||
        capture.center.blue6 != 63u ||
        capture.center.alpha6 != 63u ||
        capture.center.depth24 < UINT32_C(0x7FFFF0) ||
        capture.center.depth24 > UINT32_C(0x800010)) {
        return fail("perspective texture sample or RGB6/A6 modulation was incorrect");
    }

    if (rasterizer.render_scanline(
            192u, capture_fragment, &capture) !=
            Nds3dRasterStatus::InvalidScanline ||
        rasterizer.render_scanline(
            96u, nullptr, &capture) !=
            Nds3dRasterStatus::MissingCallback) {
        return fail("scanline argument validation failed");
    }

    std::puts("NDS 3D rasterizer tests passed");
    return 0;
}
