#include "oracis/memory/nds_arm9_protection_unit.hpp"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_arm9_protection_unit_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using oracis::memory::NdsArm9ProtectionUnit;
    using Access = NdsArm9ProtectionUnit::AccessKind;

    NdsArm9ProtectionUnit mpu;
    constexpr uint32_t regions[] = {
        UINT32_C(0x04000033), UINT32_C(0x0200002D),
        UINT32_C(0x027E0021), UINT32_C(0x08000035),
        UINT32_C(0x027E001B), UINT32_C(0x0100002F),
        UINT32_C(0xFFFF001D), UINT32_C(0x027FF017)
    };

    if (mpu.can_enable()) return fail("enabled with no programmed MPU state");
    for (uint32_t index = 0u; index < 8u; ++index) {
        if (!mpu.write_region(index, regions[index])) {
            return fail("rejected a region used by Platinum ARM9 startup");
        }
    }
    if (!mpu.write_cacheable(false, UINT32_C(0x42)) ||
        !mpu.write_cacheable(true, UINT32_C(0x42)) ||
        !mpu.write_bufferable(UINT32_C(0x02)) ||
        !mpu.write_access_permissions(
            false, true, UINT32_C(0x05100011)) ||
        !mpu.write_access_permissions(
            true, true, UINT32_C(0x15111011)) ||
        !mpu.can_enable()) {
        return fail("rejected the Platinum startup cache/permission profile");
    }

    mpu.set_enabled(true);
    if (!mpu.check_access(UINT32_C(0x02000800), 4u,
                          Access::InstructionFetch, true) ||
        !mpu.check_access(UINT32_C(0x02000800), 4u,
                          Access::DataRead, true) ||
        !mpu.check_access(UINT32_C(0x02000800), 4u,
                          Access::DataWrite, true)) {
        return fail("privileged ARM9 main-RAM access was denied");
    }
    if (mpu.check_access(UINT32_C(0x02000800), 4u,
                         Access::InstructionFetch, false) ||
        mpu.check_access(UINT32_C(0x02000800), 4u,
                         Access::DataRead, false)) {
        return fail("privileged-only main RAM was exposed to User mode");
    }

    if (mpu.check_access(UINT32_C(0x08000000), 4u,
                         Access::DataRead, true) ||
        !mpu.check_access(UINT32_C(0x08000000), 4u,
                          Access::InstructionFetch, true)) {
        return fail("Game Card region did not preserve separate data/instruction APs");
    }

    if (!mpu.check_access(UINT32_C(0xFFFF0000), 4u,
                          Access::InstructionFetch, true) ||
        !mpu.check_access(UINT32_C(0xFFFF0000), 4u,
                          Access::DataRead, true) ||
        mpu.check_access(UINT32_C(0xFFFF0000), 4u,
                         Access::DataWrite, true)) {
        return fail("high-vector region permissions are incorrect");
    }

    if (mpu.check_access(UINT32_C(0x027FFE00), 4u,
                         Access::DataRead, true) ||
        !mpu.check_access(UINT32_C(0x027FFE00), 4u,
                          Access::InstructionFetch, true)) {
        return fail("header mirror region permissions are incorrect");
    }

    NdsArm9ProtectionUnit::Attributes attrs{};
    if (!mpu.attributes(UINT32_C(0x02000800), false, attrs) ||
        attrs.region != 1u || !attrs.cacheable || !attrs.bufferable) {
        return fail("main-RAM cache/buffer attributes were not retained");
    }
    if (!mpu.attributes(UINT32_C(0x04000000), false, attrs) ||
        attrs.region != 0u || attrs.cacheable || attrs.bufferable) {
        return fail("I/O region attributes were not retained");
    }
    if (!mpu.attributes(UINT32_C(0x027FFE00), true, attrs) ||
        attrs.region != 7u) {
        return fail("region 7 did not override the larger overlapping region");
    }

    uint32_t readback = 0u;
    if (!mpu.read_region(7u, readback) || readback != regions[7] ||
        mpu.read_access_permissions(false, true) != UINT32_C(0x05100011) ||
        mpu.read_access_permissions(true, true) != UINT32_C(0x15111011) ||
        mpu.read_access_permissions(false, false) != UINT32_C(0x00001405)) {
        return fail("CP15 region/access-permission readback was incorrect");
    }

    if (mpu.write_region(0u, UINT32_C(0x04000034)) ||
        mpu.write_region(0u, UINT32_C(0x0400000B)) ||
        mpu.write_access_permissions(false, true, UINT32_C(0x00000007)) ||
        mpu.write_cacheable(false, UINT32_C(0x00000142))) {
        return fail("accepted a misaligned, reserved, or invalid CP15 value");
    }

    std::puts("ARM946E-S Platinum CP15 MPU register profile passed");
    return 0;
}
