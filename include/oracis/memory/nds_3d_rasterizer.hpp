/*
 * Nintendo DS 3D scanline rasterizer (initial fragment stage).
 *
 * Consumes the Geometry Engine's render buffer, samples captured texture
 * state and emits shaded per-pixel fragments for the scanline consumer.
 * Depth composition, translucent sorting, toon/shadow modes, fog blending
 * and the 48-line display cache are deliberately separate stages.
 */
#ifndef ORACIS_MEMORY_NDS_3D_RASTERIZER_HPP
#define ORACIS_MEMORY_NDS_3D_RASTERIZER_HPP

#include <cstdint>

#include "oracis/memory/nds_3d_texture_sampler.hpp"
#include "oracis/memory/nds_geometry_engine.hpp"

namespace oracis::memory {

enum class Nds3dRasterStatus : uint8_t {
    Ok = 0,
    InvalidScanline,
    MissingCallback,
    InvalidGeometry,
    TextureFailure,
    UnsupportedPolygonMode,
    WireframeNotSupported
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
    uint32_t depth24 = UINT32_C(0xFFFFFF);
    uint8_t polygon_id = 0u;
    bool fog_enabled = false;
    bool textured = false;
};

using Nds3dFragmentCallback = void (*)(
    void *context, const Nds3dFragment &fragment) noexcept;

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

private:
    const NdsGeometryEngine &geometry_;
    const Nds3dTextureSampler &textures_;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_3D_RASTERIZER_HPP */
