#include "oracis/memory/nds_3d_rasterizer.hpp"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_3d_color_buffer_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using namespace oracis::memory;

    Nds3dColorBuffer color;
    Nds3dFragment background{};
    background.x = 20u;
    background.y = 30u;
    background.red6 = 0u;
    background.green6 = 0u;
    background.blue6 = 63u;
    background.alpha6 = 63u;
    background.alpha5 = 31u;
    background.polygon_id = 1u;

    color.clear();
    if (!color.blend_fragment(background, true)) {
        return fail("opaque background fragment was not written");
    }
    const Nds3dColorPixel *pixel = color.pixel_at(20u, 30u);
    if (pixel == nullptr || pixel->red6 != 0u ||
        pixel->green6 != 0u || pixel->blue6 != 63u ||
        pixel->alpha5 != 31u || pixel->polygon_id != 1u) {
        return fail("opaque fragment did not replace the initial color");
    }

    Nds3dFragment translucent{};
    translucent.x = 20u;
    translucent.y = 30u;
    translucent.red6 = 63u;
    translucent.green6 = 0u;
    translucent.blue6 = 0u;
    translucent.alpha6 = 31u;
    translucent.alpha5 = 15u;
    translucent.polygon_id = 2u;
    translucent.fog_enabled = true;

    if (!color.blend_fragment(translucent, true)) {
        return fail("translucent fragment was not composited");
    }
    pixel = color.pixel_at(20u, 30u);
    if (pixel == nullptr || pixel->red6 != 31u ||
        pixel->green6 != 0u || pixel->blue6 != 31u ||
        pixel->alpha5 != 31u || pixel->polygon_id != 2u ||
        !pixel->fog_enabled) {
        return fail("DS alpha-blend equation or attribute update was incorrect");
    }

    /* Alpha blending disabled: a translucent source replaces the target. */
    color.clear(0u, 0u, 63u, 31u, 1u);
    if (!color.blend_fragment(translucent, false)) {
        return fail("non-blended translucent fragment was not accepted");
    }
    pixel = color.pixel_at(20u, 30u);
    if (pixel == nullptr || pixel->red6 != 63u ||
        pixel->green6 != 0u || pixel->blue6 != 0u ||
        pixel->alpha5 != 31u) {
        return fail("DISP3DCNT alpha-blending disable did not bypass composition");
    }

    /* An alpha-zero rear plane is overwritten rather than blended with. */
    color.clear(0u, 0u, 0u, 0u, 0u);
    if (!color.blend_fragment(translucent, true)) {
        return fail("fragment over transparent rear plane was not accepted");
    }
    pixel = color.pixel_at(20u, 30u);
    if (pixel == nullptr || pixel->red6 != 63u ||
        pixel->green6 != 0u || pixel->blue6 != 0u ||
        pixel->alpha5 != 15u) {
        return fail("transparent rear-plane bypass rule was incorrect");
    }

    /* Fully transparent fragments do not modify color or metadata. */
    Nds3dFragment transparent = translucent;
    transparent.alpha6 = 0u;
    transparent.alpha5 = 0u;
    transparent.polygon_id = 9u;
    if (color.blend_fragment(transparent, true)) {
        return fail("fully transparent fragment was reported as drawn");
    }
    pixel = color.pixel_at(20u, 30u);
    if (pixel == nullptr || pixel->red6 != 63u ||
        pixel->alpha5 != 15u || pixel->polygon_id != 2u) {
        return fail("fully transparent fragment changed the color buffer");
    }

    if (color.pixel_at(0u, 192u) != nullptr) {
        return fail("out-of-range scanline returned a color pixel");
    }

    std::puts("NDS 3D color buffer tests passed");
    return 0;
}
