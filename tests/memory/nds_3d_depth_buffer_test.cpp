#include "oracis/memory/nds_3d_rasterizer.hpp"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_3d_depth_buffer_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using namespace oracis::memory;

    Nds3dDepthBuffer depth;
    Nds3dFragment fragment{};
    fragment.x = 12u;
    fragment.y = 34u;
    fragment.alpha6 = 63u;
    fragment.depth24 = UINT32_C(0x00600000);

    depth.clear();
    if (!depth.test_and_update(fragment) ||
        depth.depth_at(12u, 34u) != UINT32_C(0x00600000)) {
        return fail("opaque fragment did not pass and update depth");
    }

    fragment.depth24 = UINT32_C(0x00700000);
    if (depth.test_and_update(fragment) ||
        depth.depth_at(12u, 34u) != UINT32_C(0x00600000)) {
        return fail("farther fragment passed the Less depth test");
    }

    fragment.depth24 = UINT32_C(0x00500000);
    if (!depth.test_and_update(fragment) ||
        depth.depth_at(12u, 34u) != UINT32_C(0x00500000)) {
        return fail("nearer fragment failed the Less depth test");
    }

    depth.clear(UINT32_C(0x00500000));
    fragment.depth_equal = true;
    fragment.depth24 = UINT32_C(0x00500200);
    if (!depth.test_and_update(fragment) ||
        depth.depth_at(12u, 34u) != UINT32_C(0x00500200)) {
        return fail("Depth Equal rejected the inclusive +/-0x200 tolerance boundary");
    }

    depth.clear(UINT32_C(0x00500000));
    fragment.depth24 = UINT32_C(0x00500201);
    if (depth.test_and_update(fragment)) {
        return fail("Depth Equal accepted a value outside the +/-0x200 tolerance");
    }

    depth.clear(UINT32_C(0x00500000));
    fragment.depth_equal = false;
    fragment.depth24 = UINT32_C(0x00500000);
    if (depth.test_and_update(fragment)) {
        return fail("Less depth test accepted an exactly equal value");
    }

    depth.clear();
    fragment.depth24 = UINT32_C(0x00400000);
    fragment.alpha6 = 30u;
    fragment.translucent_depth_write = false;
    if (!depth.test_and_update(fragment) ||
        depth.depth_at(12u, 34u) != Nds3dDepthBuffer::MaximumDepth) {
        return fail("translucent fragment wrote depth with depth-write disabled");
    }

    fragment.translucent_depth_write = true;
    if (!depth.test_and_update(fragment) ||
        depth.depth_at(12u, 34u) != UINT32_C(0x00400000)) {
        return fail("translucent fragment did not write depth when enabled");
    }

    depth.clear();
    fragment.alpha6 = 0u;
    fragment.depth24 = UINT32_C(0x00100000);
    if (depth.test_and_update(fragment) ||
        depth.depth_at(12u, 34u) != Nds3dDepthBuffer::MaximumDepth) {
        return fail("fully transparent fragment affected depth");
    }

    fragment.alpha6 = 63u;
    fragment.y = 192u;
    if (depth.test_and_update(fragment)) {
        return fail("invalid scanline changed the depth buffer");
    }

    std::puts("NDS 3D depth buffer tests passed");
    return 0;
}
