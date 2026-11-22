#include "oracis/memory/guest_memory.hpp"
#include "oracis/memory/nds_3d_rasterizer.hpp"
#include "oracis/memory/nds_3d_texture_sampler.hpp"
#include "oracis/memory/nds_display_control.hpp"
#include "oracis/memory/nds_geometry_engine.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"
#include "oracis/memory/nds_vram_control.hpp"

#include <array>
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

bool write_rear_bitmap_pixel(
    oracis::memory::GuestMemory &memory,
    uint32_t lcdc_base,
    uint8_t row,
    uint8_t column,
    uint16_t value)
{
    const uint32_t pixel_index =
        static_cast<uint32_t>(row) * UINT32_C(256) + column;
    return write_halfword(
        memory, lcdc_base + pixel_index * 2u, value);
}

struct Capture {
    uint32_t count = 0u;
    uint32_t center_fragment_count = 0u;
    bool center_seen = false;
    bool decal_transparent_texel_center_seen = false;
    bool transparent_modulate_texel_center_seen = false;
    bool w_buffer_center_seen = false;
    oracis::memory::Nds3dFragment center{};
    oracis::memory::Nds3dFragment decal_center{};
    oracis::memory::Nds3dFragment w_buffer_center{};
};

void capture_fragment(
    void *context,
    const oracis::memory::Nds3dFragment &fragment) noexcept
{
    auto *capture = static_cast<Capture *>(context);
    ++capture->count;
    if (fragment.x == 127u && fragment.y == 96u) {
        ++capture->center_fragment_count;
        if (fragment.polygon_id == 1u && !capture->center_seen) {
            capture->center_seen = true;
            capture->center = fragment;
        }
        if (fragment.polygon_id == 2u) {
            capture->decal_transparent_texel_center_seen = true;
            capture->decal_center = fragment;
        }
        if (fragment.polygon_id == 3u) {
            capture->transparent_modulate_texel_center_seen = true;
        }
        if (fragment.polygon_id == 4u) {
            capture->w_buffer_center_seen = true;
            capture->w_buffer_center = fragment;
        }
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
    NdsDisplayControl display(interrupts);
    if (geometry.install() != Status::Ok ||
        vram.install() != Status::Ok ||
        display.install() != Status::Ok) {
        return fail("geometry, VRAM or display-control device installation failed");
    }

    /* Prepare texture slot 0 plus the color/depth bitmap slots 2 and 3. */
    if (vram.write(
            NDS_ARM9, NdsVramControl::VramCntAAddress, 1u,
            UINT32_C(0x80)) != Status::Ok ||
        vram.write(
            NDS_ARM9, NdsVramControl::VramCntCAddress, 1u,
            UINT32_C(0x80)) != Status::Ok ||
        vram.write(
            NDS_ARM9, NdsVramControl::VramCntDAddress, 1u,
            UINT32_C(0x80)) != Status::Ok ||
        !write_halfword(memory, UINT32_C(0x06800000), UINT16_C(0xFC00)) ||
        !write_halfword(memory, UINT32_C(0x06800006), UINT16_C(0xFC00)) ||
        !write_halfword(memory, UINT32_C(0x0680000A), UINT16_C(0x801F)) ||
        !write_halfword(memory, UINT32_C(0x0680000E), UINT16_C(0x7FFF)) ||
        !write_rear_bitmap_pixel(
            memory, UINT32_C(0x06840000), 250u, 120u, UINT16_C(0x801F)) ||
        !write_rear_bitmap_pixel(
            memory, UINT32_C(0x06840000), 250u, 121u, UINT16_C(0x03E0)) ||
        !write_rear_bitmap_pixel(
            memory, UINT32_C(0x06840000), 0u, 120u, UINT16_C(0xFC00)) ||
        !write_rear_bitmap_pixel(
            memory, UINT32_C(0x06840000), 0u, 0u, UINT16_C(0xFC1F)) ||
        !write_rear_bitmap_pixel(
            memory, UINT32_C(0x06840000), 185u, 119u, UINT16_C(0x83E0)) ||
        !write_rear_bitmap_pixel(
            memory, UINT32_C(0x06860000), 250u, 120u, UINT16_C(0xFFFF)) ||
        !write_rear_bitmap_pixel(
            memory, UINT32_C(0x06860000), 250u, 121u, UINT16_C(0x4001)) ||
        !write_rear_bitmap_pixel(
            memory, UINT32_C(0x06860000), 0u, 120u, UINT16_C(0xC000)) ||
        !write_rear_bitmap_pixel(
            memory, UINT32_C(0x06860000), 0u, 0u, UINT16_C(0x0001)) ||
        !write_rear_bitmap_pixel(
            memory, UINT32_C(0x06860000), 185u, 119u, UINT16_C(0x9234)) ||
        vram.write(
            NDS_ARM9, NdsVramControl::VramCntAAddress, 1u,
            UINT32_C(0x83)) != Status::Ok ||
        vram.write(
            NDS_ARM9, NdsVramControl::VramCntCAddress, 1u,
            UINT32_C(0x93)) != Status::Ok ||
        vram.write(
            NDS_ARM9, NdsVramControl::VramCntDAddress, 1u,
            UINT32_C(0x9B)) != Status::Ok) {
        return fail("direct-color texture setup failed");
    }

    /*
     * Make clip W depend on vertex Z while clip Z remains zero. The three
     * vertices then have different W values, so affine and perspective-
     * corrected texture interpolation select different direct-color texels.
     */
    if (!write_command(geometry, 0x10u, 0u)) {
        return fail("projection matrix mode selection failed");
    }
    geometry.advance_bus_cycles(1u);
    constexpr std::array<uint32_t, 16> perspective_projection = {
        UINT32_C(0x1000), 0u, 0u, 0u,
        0u, UINT32_C(0x1000), 0u, 0u,
        0u, 0u, 0u, UINT32_C(0x1000),
        0u, 0u, 0u, 0u
    };
    for (uint32_t parameter : perspective_projection) {
        if (!write_command(geometry, 0x16u, parameter)) {
            return fail("perspective projection matrix parameter failed");
        }
    }
    geometry.advance_bus_cycles(34u);

    /* One front-facing opaque triangle, polygon ID 1. */
    if (!write_command(geometry, 0x29u,
            (UINT32_C(31) << 16u) | (UINT32_C(1) << 7u) |
                (UINT32_C(1) << 24u))) {
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

    const std::array<uint32_t, 3> positions = {
        encode_vtx10(-64, -64, 64),
        encode_vtx10(128, -128, 128),
        encode_vtx10(0, 256, 256)
    };
    constexpr std::array<uint16_t, 3> texture_s{
        0u, 64u, 128u
    };
    for (unsigned index = 0u; index < positions.size(); ++index) {
        if (!write_command(geometry, 0x22u, texture_s[index])) {
            return fail("TEXCOORD enqueue failed");
        }
        geometry.advance_bus_cycles(1u);
        if (!write_command(geometry, 0x20u, UINT32_C(0x7FFF))) {
            return fail("COLOR enqueue failed");
        }
        geometry.advance_bus_cycles(1u);
        if (!write_command(geometry, 0x24u, positions[index])) {
            return fail("VTX_10 enqueue failed");
        }
        geometry.advance_bus_cycles(8u);
    }

    if (geometry.polygon_count() != 1u) {
        return fail("opaque triangle was not assembled");
    }

    /*
     * Decal mode samples texel 7, whose direct-color alpha bit is clear.
     * This uses the vertex RGB and alpha rather than discarding the texel.
     */
    if (!write_command(geometry, 0x29u,
            (UINT32_C(31) << 16u) | (UINT32_C(1) << 7u) |
                (UINT32_C(1) << 4u) | (UINT32_C(2) << 24u))) {
        return fail("decal POLYGON_ATTR enqueue failed");
    }
    geometry.advance_bus_cycles(1u);
    if (!write_command(geometry, 0x40u, 0u)) {
        return fail("transparent BEGIN_VTXS enqueue failed");
    }
    geometry.advance_bus_cycles(1u);
    for (unsigned index = 0u; index < positions.size(); ++index) {
        if (!write_command(geometry, 0x22u, UINT32_C(256))) {
            return fail("transparent TEXCOORD enqueue failed");
        }
        geometry.advance_bus_cycles(1u);
        if (!write_command(geometry, 0x20u, UINT32_C(0x7FFF))) {
            return fail("transparent COLOR enqueue failed");
        }
        geometry.advance_bus_cycles(1u);
        if (!write_command(geometry, 0x24u, positions[index])) {
            return fail("transparent VTX_10 enqueue failed");
        }
        geometry.advance_bus_cycles(8u);
    }

    /*
     * A third overlapping polygon uses Modulation with the same transparent
     * texel. Its final alpha is zero and it must not reach the callback.
     */
    if (!write_command(geometry, 0x29u,
            (UINT32_C(31) << 16u) | (UINT32_C(1) << 7u) |
                (UINT32_C(3) << 24u))) {
        return fail("modulation POLYGON_ATTR enqueue failed");
    }
    geometry.advance_bus_cycles(1u);
    if (!write_command(geometry, 0x40u, 0u)) {
        return fail("modulation BEGIN_VTXS enqueue failed");
    }
    geometry.advance_bus_cycles(1u);
    for (unsigned index = 0u; index < positions.size(); ++index) {
        if (!write_command(geometry, 0x22u, UINT32_C(256))) {
            return fail("modulation TEXCOORD enqueue failed");
        }
        geometry.advance_bus_cycles(1u);
        if (!write_command(geometry, 0x20u, UINT32_C(0x7FFF))) {
            return fail("modulation COLOR enqueue failed");
        }
        geometry.advance_bus_cycles(1u);
        if (!write_command(geometry, 0x24u, positions[index])) {
            return fail("modulation VTX_10 enqueue failed");
        }
        geometry.advance_bus_cycles(8u);
    }

    if (geometry.polygon_count() != 3u ||
        !write_command(geometry, 0x50u, UINT32_C(3))) {
        return fail("three triangles or SWAP_BUFFERS enqueue failed");
    }
    geometry.advance_bus_cycles(1u);
    if (!geometry.swap_waiting_for_vblank()) {
        return fail("SWAP_BUFFERS did not wait for VBlank");
    }
    geometry.notify_vblank();
    geometry.advance_bus_cycles(392u);
    if (geometry.render_polygon_count() != 3u ||
        geometry.render_vertex_count() != 9u ||
        geometry.render_swap_parameters() != 0u ||
        geometry.swap_parameters() != 3u) {
        return fail("SWAP_BUFFERS did not preserve render-buffer mode separately from next-build mode");
    }

    Nds3dTextureSampler textures(vram);
    Nds3dRasterizer rasterizer(geometry, textures);
    Capture capture{};
    if (rasterizer.render_scanline(
            96u, capture_fragment, &capture) != Nds3dRasterStatus::Ok ||
        capture.count == 0u || !capture.center_seen ||
        capture.center_fragment_count != 2u ||
        !capture.decal_transparent_texel_center_seen ||
        capture.transparent_modulate_texel_center_seen) {
        return fail("scanline rasterizer emitted a transparent texel or missed the opaque fragment");
    }

    /*
     * The first render buffer uses automatic translucent sorting. Until that
     * sort exists, the color compositor must fail closed without changing
     * color or depth.
     */
    Nds3dDepthBuffer auto_sort_depth{};
    Nds3dColorBuffer auto_sort_color{};
    if (rasterizer.render_scanline_composited(
            96u, auto_sort_depth, auto_sort_color,
            true, false, 0u) !=
            Nds3dRasterStatus::TranslucentSortingNotSupported ||
        auto_sort_depth.depth_at(127u, 96u) !=
            Nds3dDepthBuffer::MaximumDepth ||
        auto_sort_color.pixel_at(127u, 96u)->alpha5 != 0u) {
        return fail("automatic translucent sorting did not fail closed");
    }

    if (!capture.center.textured ||
        capture.center.red6 != 0u ||
        capture.center.green6 != 0u ||
        capture.center.blue6 != 63u ||
        capture.center.alpha6 != 63u ||
        !capture.decal_center.textured ||
        capture.decal_center.red6 != 63u ||
        capture.decal_center.green6 != 63u ||
        capture.decal_center.blue6 != 63u ||
        capture.decal_center.alpha6 != 63u ||
        capture.decal_center.polygon_id != 2u ||
        capture.center.depth_equal ||
        capture.center.translucent_depth_write ||
        capture.center.depth24 < UINT32_C(0x7FFFF0) ||
        capture.center.depth24 > UINT32_C(0x800010)) {
        return fail("perspective sample, modulation discard or decal vertex-color fallback was incorrect");
    }

    if (rasterizer.render_scanline(
            192u, capture_fragment, &capture) !=
            Nds3dRasterStatus::InvalidScanline ||
        rasterizer.render_scanline(
            96u, nullptr, &capture) !=
            Nds3dRasterStatus::MissingCallback) {
        return fail("scanline argument validation failed");
    }

    /*
     * Build a second frame after the first SWAP_BUFFERS selected W buffering.
     * The second swap requests Z for the following build, but the published
     * render buffer must retain the W option used while this polygon was built.
     */
    if (!write_command(geometry, 0x29u,
            (UINT32_C(31) << 16u) | (UINT32_C(1) << 7u) |
                (UINT32_C(4) << 24u))) {
        return fail("W-buffer POLYGON_ATTR enqueue failed");
    }
    geometry.advance_bus_cycles(1u);
    if (!write_command(geometry, 0x40u, 0u)) {
        return fail("W-buffer BEGIN_VTXS enqueue failed");
    }
    geometry.advance_bus_cycles(1u);
    for (unsigned index = 0u; index < positions.size(); ++index) {
        if (!write_command(geometry, 0x22u, texture_s[index])) {
            return fail("W-buffer TEXCOORD enqueue failed");
        }
        geometry.advance_bus_cycles(1u);
        if (!write_command(geometry, 0x20u, UINT32_C(0x7FFF))) {
            return fail("W-buffer COLOR enqueue failed");
        }
        geometry.advance_bus_cycles(1u);
        if (!write_command(geometry, 0x24u, positions[index])) {
            return fail("W-buffer VTX_10 enqueue failed");
        }
        geometry.advance_bus_cycles(8u);
    }
    if (geometry.polygon_count() != 1u ||
        !write_command(geometry, 0x50u, 0u)) {
        return fail("W-buffer polygon or second SWAP_BUFFERS enqueue failed");
    }
    geometry.advance_bus_cycles(1u);
    if (!geometry.swap_waiting_for_vblank()) {
        return fail("W-buffer SWAP_BUFFERS did not wait for VBlank");
    }
    geometry.notify_vblank();
    geometry.advance_bus_cycles(392u);
    if (geometry.render_polygon_count() != 1u ||
        geometry.render_swap_parameters() != 3u ||
        geometry.swap_parameters() != 0u) {
        return fail("W-buffer/manual-sort mode was not retained with the second render buffer");
    }

    Nds3dDepthBuffer alpha_test_depth{};
    Nds3dColorBuffer alpha_test_color{};
    if (display.write(
            NDS_ARM9, NdsDisplayControl::Disp3dControlAddress, 2u,
            static_cast<uint32_t>(
                NdsDisplayControl::Disp3dAlphaTestEnable |
                NdsDisplayControl::Disp3dAlphaBlendEnable)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::AlphaTestReferenceAddress,
            1u, UINT32_C(31)) != Status::Ok) {
        return fail("DISP3DCNT/ALPHA_TEST_REF setup failed");
    }

    if (rasterizer.render_scanline_composited(
            96u, alpha_test_depth, alpha_test_color, display) !=
            Nds3dRasterStatus::Ok ||
        alpha_test_depth.depth_at(127u, 96u) !=
            Nds3dDepthBuffer::MaximumDepth ||
        alpha_test_color.pixel_at(127u, 96u)->alpha5 != 0u) {
        return fail("MMIO alpha-test rejection updated color or depth");
    }

    if (display.write(
            NDS_ARM9, NdsDisplayControl::AlphaTestReferenceAddress,
            1u, UINT32_C(30)) != Status::Ok) {
        return fail("ALPHA_TEST_REF update failed");
    }

    constexpr uint32_t rear_clear_color =
        (UINT32_C(31) << 0u) |
        (UINT32_C(16) << 5u) |
        (UINT32_C(4) << 10u) |
        (UINT32_C(1) << 15u) |
        (UINT32_C(12) << 16u) |
        (UINT32_C(37) << 24u);
    if (display.write(
            NDS_ARM9, NdsDisplayControl::ClearColorAddress, 4u,
            rear_clear_color) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::ClearDepthAddress, 2u,
            UINT16_C(0x4000)) != Status::Ok ||
        display.write(
            NDS_ARM9, NdsDisplayControl::ClearImageOffsetAddress, 2u,
            UINT16_C(0xFA78)) != Status::Ok ||
        display.three_d_clear_depth_expanded24() != UINT32_C(0x800000)) {
        return fail("rear-plane clear register setup or depth expansion failed");
    }

    Nds3dDepthBuffer depth{};
    Nds3dColorBuffer color{};
    if (rasterizer.initialize_rear_plane(
            color, depth, display) != Nds3dRasterStatus::Ok) {
        return fail("uniform rear-plane initialization failed");
    }
    const Nds3dColorPixel *rear_pixel = color.pixel_at(0u, 0u);
    if (rear_pixel == nullptr ||
        rear_pixel->red6 != 63u ||
        rear_pixel->green6 != 33u ||
        rear_pixel->blue6 != 9u ||
        rear_pixel->alpha5 != 12u ||
        rear_pixel->polygon_id != 37u ||
        !rear_pixel->fog_enabled ||
        depth.depth_at(0u, 0u) != UINT32_C(0x800000)) {
        return fail("CLEAR_COLOR RGB6/A5/fog/ID or CLEAR_DEPTH expansion was incorrect");
    }

    /*
     * Slot 2 stores RGB555/binary alpha, slot 3 stores depth15/fog. Losing
     * either source must leave the previously initialized plane untouched.
     */
    if (display.write(
            NDS_ARM9, NdsDisplayControl::Disp3dControlAddress, 2u,
            static_cast<uint32_t>(
                NdsDisplayControl::Disp3dAlphaTestEnable |
                NdsDisplayControl::Disp3dAlphaBlendEnable |
                (UINT16_C(1u) << 14u))) != Status::Ok ||
        vram.write(
            NDS_ARM9, NdsVramControl::VramCntDAddress, 1u, 0u) != Status::Ok ||
        rasterizer.initialize_rear_plane(
            color, depth, display) != Nds3dRasterStatus::TextureFailure ||
        color.pixel_at(0u, 0u)->red6 != 63u ||
        color.pixel_at(0u, 0u)->polygon_id != 37u ||
        depth.depth_at(0u, 0u) != UINT32_C(0x800000) ||
        vram.write(
            NDS_ARM9, NdsVramControl::VramCntDAddress, 1u,
            UINT32_C(0x9B)) != Status::Ok) {
        return fail("missing slot did not fail without mutating the rear plane");
    }

    if (rasterizer.initialize_rear_plane(
            color, depth, display) != Nds3dRasterStatus::Ok) {
        return fail("rear-plane bitmap initialization failed");
    }

    const Nds3dColorPixel *bitmap_red = color.pixel_at(0u, 0u);
    const Nds3dColorPixel *bitmap_transparent = color.pixel_at(1u, 0u);
    const Nds3dColorPixel *bitmap_blue_wrap = color.pixel_at(0u, 6u);
    const Nds3dColorPixel *bitmap_xy_wrap = color.pixel_at(136u, 6u);
    const Nds3dColorPixel *bitmap_bottom_wrap = color.pixel_at(255u, 191u);
    if (bitmap_red == nullptr ||
        bitmap_red->red6 != 63u || bitmap_red->green6 != 0u ||
        bitmap_red->blue6 != 0u || bitmap_red->alpha5 != 31u ||
        bitmap_red->polygon_id != 37u || !bitmap_red->fog_enabled ||
        depth.depth_at(0u, 0u) != UINT32_C(0x00FFFFFF) ||
        bitmap_transparent == nullptr ||
        bitmap_transparent->green6 != 63u ||
        bitmap_transparent->alpha5 != 0u ||
        bitmap_transparent->polygon_id != 37u ||
        bitmap_transparent->fog_enabled ||
        depth.depth_at(1u, 0u) != UINT32_C(0x00800200) ||
        bitmap_blue_wrap == nullptr ||
        bitmap_blue_wrap->blue6 != 63u ||
        bitmap_blue_wrap->alpha5 != 31u ||
        !bitmap_blue_wrap->fog_enabled ||
        depth.depth_at(0u, 6u) != UINT32_C(0x00800000) ||
        bitmap_xy_wrap == nullptr ||
        bitmap_xy_wrap->red6 != 63u ||
        bitmap_xy_wrap->blue6 != 63u ||
        bitmap_xy_wrap->fog_enabled ||
        depth.depth_at(136u, 6u) != UINT32_C(0x00000200) ||
        bitmap_bottom_wrap == nullptr ||
        bitmap_bottom_wrap->green6 != 63u ||
        bitmap_bottom_wrap->alpha5 != 31u ||
        !bitmap_bottom_wrap->fog_enabled ||
        depth.depth_at(255u, 191u) != UINT32_C(0x00246800)) {
        return fail("rear-plane bitmap color/depth/fog/alpha or wrap failed");
    }

    /* Restore the uniform rear-plane before the polygon composition test. */
    if (display.write(
            NDS_ARM9, NdsDisplayControl::Disp3dControlAddress, 2u,
            static_cast<uint32_t>(
                NdsDisplayControl::Disp3dAlphaTestEnable |
                NdsDisplayControl::Disp3dAlphaBlendEnable)) != Status::Ok ||
        rasterizer.initialize_rear_plane(
            color, depth, display) != Nds3dRasterStatus::Ok) {
        return fail("uniform rear-plane mode restore failed");
    }
    if (rasterizer.render_scanline_composited(
            96u, depth, color, display) != Nds3dRasterStatus::Ok) {
        return fail("manual-order MMIO-composited scanline returned an error");
    }
    const Nds3dColorPixel *center_pixel = color.pixel_at(127u, 96u);
    if (center_pixel == nullptr ||
        center_pixel->blue6 != 63u ||
        center_pixel->alpha5 != 31u ||
        center_pixel->polygon_id != 4u) {
        return fail("opaque W-buffer fragment was not composited into RGB6/A5");
    }
    Capture w_capture{};
    if (rasterizer.render_scanline(
            96u, capture_fragment, &w_capture) != Nds3dRasterStatus::Ok ||
        !w_capture.w_buffer_center_seen ||
        !w_capture.w_buffer_center.w_buffer ||
        w_capture.w_buffer_center.depth24 == 0u ||
        w_capture.w_buffer_center.depth24 >= UINT32_C(0x7FFFF0) ||
        depth.depth_at(127u, 96u) != w_capture.w_buffer_center.depth24) {
        return fail("W-buffer perspective depth was not retained after composition");
    }

    /*
     * POLYGON_ATTR alpha=0 selects wireframe mode. The current line/edge
     * rasterizer is intentionally incomplete and must report this explicitly.
     * The second SWAP selected Z for this third build buffer.
     */
    if (!write_command(geometry, 0x29u,
            (UINT32_C(1) << 7u) | (UINT32_C(3) << 24u))) {
        return fail("wireframe POLYGON_ATTR enqueue failed");
    }
    geometry.advance_bus_cycles(1u);
    if (!write_command(geometry, 0x40u, 0u)) {
        return fail("wireframe BEGIN_VTXS enqueue failed");
    }
    geometry.advance_bus_cycles(1u);
    for (unsigned index = 0u; index < positions.size(); ++index) {
        if (!write_command(geometry, 0x22u, texture_s[index])) {
            return fail("wireframe TEXCOORD enqueue failed");
        }
        geometry.advance_bus_cycles(1u);
        if (!write_command(geometry, 0x20u, UINT32_C(0x7FFF))) {
            return fail("wireframe COLOR enqueue failed");
        }
        geometry.advance_bus_cycles(1u);
        if (!write_command(geometry, 0x24u, positions[index])) {
            return fail("wireframe VTX_10 enqueue failed");
        }
        geometry.advance_bus_cycles(8u);
    }
    if (geometry.polygon_count() != 1u ||
        !write_command(geometry, 0x50u, 0u)) {
        return fail("wireframe polygon or third SWAP_BUFFERS enqueue failed");
    }
    geometry.advance_bus_cycles(1u);
    if (!geometry.swap_waiting_for_vblank()) {
        return fail("wireframe SWAP_BUFFERS did not wait for VBlank");
    }
    geometry.notify_vblank();
    geometry.advance_bus_cycles(392u);
    if (geometry.render_polygon_count() != 1u ||
        geometry.render_swap_parameters() != 0u ||
        rasterizer.render_scanline(
            96u, capture_fragment, &capture) !=
            Nds3dRasterStatus::WireframeNotSupported) {
        return fail("POLYGON_ATTR alpha zero was not identified as wireframe");
    }

    std::puts("NDS 3D rasterizer tests passed");
    return 0;
}
