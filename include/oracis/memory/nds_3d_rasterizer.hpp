/*
 * Nintendo DS 3D scanline rasterizer and BG0 scanline producer.
 *
 * Consumes the Geometry Engine's render buffer, samples captured texture
 * state, composes fragments into RGB6/A5 staging and exports BG0 RGB555/
 * alpha5 scanlines to the bounded 48-line FIFO. MMIO-controlled fog is
 * applied after scanline composition. Translucent Y-sort, toon/shadow,
 * wireframe/edge marking and cycle-accurate scheduling remain open.
 */
#ifndef ORACIS_MEMORY_NDS_3D_RASTERIZER_HPP
#define ORACIS_MEMORY_NDS_3D_RASTERIZER_HPP

#include <array>
#include <cstdint>

#include "oracis/memory/nds_3d_texture_sampler.hpp"
#include "oracis/memory/nds_3d_scanline_cache.hpp"
#include "oracis/memory/nds_geometry_engine.hpp"

namespace oracis::memory {

class NdsDisplayControl;

enum class Nds3dRasterStatus : uint8_t {
    Ok = 0,
    InvalidScanline,
    MissingCallback,
    InvalidGeometry,
    TextureFailure,
    UnsupportedPolygonMode,
    WireframeNotSupported,
    TranslucentSortingNotSupported,
    InvalidAlphaTestReference,
    DisplayControlUnavailable,
    ScanlineCacheFull,
    ScanlineCacheOutOfOrder,
    ScanlineCacheInvalid
};

/*
 * Fragment color uses the Nintendo DS renderer's six-bit channel domain.
 * Depth is normalized to 0..0xFFFFFF. The consumer still performs depth
 * testing, translucent ordering and line-cache storage.
 */
struct Nds3dFragment {
    uint8_t x = 0u;
    uint8_t y = 0u;
    uint8_t red6 = 0u;
    uint8_t green6 = 0u;
    uint8_t blue6 = 0u;
    uint8_t alpha6 = 0u;
    uint8_t alpha5 = 0u;
    uint32_t depth24 = UINT32_C(0xFFFFFF);
    uint8_t polygon_id = 0u;
    bool fog_enabled = false;
    bool textured = false;
    bool depth_equal = false;
    bool translucent = false;
    bool translucent_depth_write = false;
    bool w_buffer = false;
};

using Nds3dFragmentCallback = void (*)(
    void *context, const Nds3dFragment &fragment) noexcept;

/*
 * Per-pixel 24-bit depth comparison for the 256x192 DS 3D target.
 * Fragments are submitted in the caller's current polygon order; this stage
 * does not sort polygons or blend colors.
 */
class Nds3dDepthBuffer final {
public:
    static constexpr uint32_t ScreenWidth = 256u;
    static constexpr uint32_t ScreenHeight = 192u;
    static constexpr uint32_t MaximumDepth = UINT32_C(0x00FFFFFF);
    static constexpr uint32_t DepthEqualTolerance = UINT32_C(0x200);
    static constexpr uint32_t WDepthEqualTolerance = UINT32_C(0x0FF);

    Nds3dDepthBuffer() noexcept { clear(); }
    Nds3dDepthBuffer(const Nds3dDepthBuffer &) = delete;
    Nds3dDepthBuffer &operator=(const Nds3dDepthBuffer &) = delete;

    void clear(uint32_t depth = MaximumDepth) noexcept;
    bool write_depth(uint8_t x, uint8_t y, uint32_t depth) noexcept;
    bool test_and_update(const Nds3dFragment &fragment) noexcept;
    uint32_t depth_at(uint8_t x, uint8_t y) const noexcept;

private:
    std::array<uint32_t, ScreenWidth * ScreenHeight> depths_{};
};

struct Nds3dColorPixel {
    uint8_t red6 = 0u;
    uint8_t green6 = 0u;
    uint8_t blue6 = 0u;
    uint8_t alpha5 = 0u;
    uint8_t polygon_id = 0u;
    bool fog_enabled = false;
};

/*
 * Host-side 256x192 color staging buffer for the DS 3D output.
 * Implements the DS polygon/framebuffer alpha equation. Polygon sorting and
 * the hardware's 48-line cache remain separate stages.
 */
class Nds3dColorBuffer final {
public:
    static constexpr uint32_t ScreenWidth = 256u;
    static constexpr uint32_t ScreenHeight = 192u;

    Nds3dColorBuffer() noexcept { clear(); }
    Nds3dColorBuffer(const Nds3dColorBuffer &) = delete;
    Nds3dColorBuffer &operator=(const Nds3dColorBuffer &) = delete;

    void clear(
        uint8_t red6 = 0u,
        uint8_t green6 = 0u,
        uint8_t blue6 = 0u,
        uint8_t alpha5 = 0u,
        uint8_t polygon_id = 0u,
        bool fog_enabled = false) noexcept;
    bool write_pixel(
        uint8_t x, uint8_t y, const Nds3dColorPixel &pixel) noexcept;
    bool blend_fragment(
        const Nds3dFragment &fragment,
        bool alpha_blending_enabled) noexcept;
    bool apply_fog_scanline(
        uint16_t scanline,
        const Nds3dDepthBuffer &depth_buffer,
        const NdsDisplayControl &display_control) noexcept;
    const Nds3dColorPixel *pixel_at(
        uint8_t x, uint8_t y) const noexcept;
    bool copy_bg0_scanline(
        uint16_t scanline, Nds3dBg0Scanline &output) const noexcept;

private:
    std::array<Nds3dColorPixel, ScreenWidth * ScreenHeight> pixels_{};
};

class Nds3dRasterizer final {
public:
    static constexpr uint32_t ScreenWidth = 256u;
    static constexpr uint32_t ScreenHeight = 192u;
    static constexpr uint32_t MaximumDepth = UINT32_C(0x00FFFFFF);

    Nds3dRasterizer(
        const NdsGeometryEngine &geometry,
        const Nds3dTextureSampler &textures) noexcept
        : geometry_(geometry), textures_(textures) {}

    Nds3dRasterizer(const Nds3dRasterizer &) = delete;
    Nds3dRasterizer &operator=(const Nds3dRasterizer &) = delete;

    /*
     * Rasterize the selected line from the currently swapped render buffer.
     * Coordinates are tested at pixel centers; clipped geometry and polygon
     * face visibility are supplied by NdsGeometryEngine.
     */
    Nds3dRasterStatus render_scanline(
        uint16_t scanline,
        Nds3dFragmentCallback callback,
        void *context) const noexcept;

    /*
     * Rasterize and forward only fragments that pass the scanline depth
     * buffer. The caller owns the depth buffer lifetime and clear policy.
     */
    Nds3dRasterStatus render_scanline_depth_tested(
        uint16_t scanline,
        Nds3dDepthBuffer &depth_buffer,
        Nds3dFragmentCallback callback,
        void *context) const noexcept;

    /*
     * Composite one scanline into color/depth staging buffers. Solid-only
     * buffers render with either SWAP_BUFFERS sort setting; translucent
     * alpha 1..30 still requires manual order until automatic DS Y-sorting
     * is implemented.
     */
    Nds3dRasterStatus render_scanline_composited(
        uint16_t scanline,
        Nds3dDepthBuffer &depth_buffer,
        Nds3dColorBuffer &color_buffer,
        bool alpha_blending_enabled,
        bool alpha_test_enabled,
        uint8_t alpha_test_reference) const noexcept;

    /*
     * Initialize the rear-plane from CLEAR_COLOR/CLEAR_DEPTH, or from the
     * 256x256 color/depth bitmaps in texture slots 2/3 when DISP3DCNT.Bit14
     * is set. The visible 256x192 window is copied to the staging surfaces.
     */
    Nds3dRasterStatus initialize_rear_plane(
        Nds3dColorBuffer &color_buffer,
        Nds3dDepthBuffer &depth_buffer,
        const NdsDisplayControl &display_control) const noexcept;

    /*
     * Pull DISP3DCNT and ALPHA_TEST_REF directly from the display MMIO device.
     */
    Nds3dRasterStatus render_scanline_composited(
        uint16_t scanline,
        Nds3dDepthBuffer &depth_buffer,
        Nds3dColorBuffer &color_buffer,
        const NdsDisplayControl &display_control) const noexcept;

    /*
     * Render one line and enqueue RGB555/alpha5 for Engine A BG0. The caller
     * initializes the rear-plane once per frame and resets the FIFO before
     * pre-rendering; cache capacity/order errors are reported explicitly.
     */
    Nds3dRasterStatus render_scanline_to_cache(
        uint16_t scanline,
        Nds3dDepthBuffer &depth_buffer,
        Nds3dColorBuffer &color_buffer,
        Nds3dScanlineCache &cache,
        const NdsDisplayControl &display_control) const noexcept;

private:
    Nds3dRasterStatus render_scanline_impl(
        uint16_t scanline,
        Nds3dFragmentCallback callback,
        void *context,
        const std::array<uint16_t, 32> *toon_table,
        bool highlight_shading) const noexcept;
    Nds3dRasterStatus render_scanline_composited_impl(
        uint16_t scanline,
        Nds3dDepthBuffer &depth_buffer,
        Nds3dColorBuffer &color_buffer,
        bool alpha_blending_enabled,
        bool alpha_test_enabled,
        uint8_t alpha_test_reference,
        const std::array<uint16_t, 32> *toon_table,
        bool highlight_shading) const noexcept;

    const NdsGeometryEngine &geometry_;
    const Nds3dTextureSampler &textures_;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_3D_RASTERIZER_HPP */
