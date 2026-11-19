#include "oracis/recomp/runtime_arm.h"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "runtime_mul_cycles_test: %s\n", message);
    return 1;
}

bool check(
    uint32_t multiplier,
    uint32_t signed_variant,
    uint32_t extra,
    uint32_t expected)
{
    const uint32_t actual = runtime_mul_cycles(
        multiplier, signed_variant, extra);
    if (actual == expected) return true;
    std::fprintf(stderr,
        "runtime_mul_cycles_test: value=0x%08X signed=%u extra=%u "
        "expected=%u actual=%u\n",
        multiplier, signed_variant, extra, expected, actual);
    return false;
}

} // namespace

int main()
{
    ArmCpuState arm9{};
    ArmCpuState arm7{};
    arm9.cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7.cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (!oracis_arm_runtime_bind_cpus(&arm9, &arm7)) {
        return fail("CPU binding failed");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM7)) {
        return fail("ARM7 selection failed");
    }

    /* Signed ARM MUL/MLA/SMULL/SMLAL: sign-extended byte/halfword/24-bit. */
    if (!check(UINT32_C(0x00000000), 1u, 0u, 1u) ||
        !check(UINT32_C(0xFFFFFF80), 1u, 0u, 1u) ||
        !check(UINT32_C(0x00000100), 1u, 0u, 2u) ||
        !check(UINT32_C(0xFFFF8000), 1u, 0u, 2u) ||
        !check(UINT32_C(0x00800000), 1u, 0u, 3u) ||
        !check(UINT32_C(0xFF800000), 1u, 0u, 3u) ||
        !check(UINT32_C(0x7F000000), 1u, 0u, 4u)) {
        return fail("signed early-termination thresholds were incorrect");
    }

    /* Unsigned Thumb MUL/UMULL/UMLAL: only leading zero bytes terminate. */
    if (!check(UINT32_C(0x00000080), 0u, 0u, 1u) ||
        !check(UINT32_C(0x00000100), 0u, 0u, 2u) ||
        !check(UINT32_C(0x00010000), 0u, 0u, 3u) ||
        !check(UINT32_C(0x01000000), 0u, 0u, 4u) ||
        !check(UINT32_C(0xFFFFFF00), 0u, 0u, 4u)) {
        return fail("unsigned early-termination thresholds were incorrect");
    }

    /* Codegen adds 0/1/2 internal cycles for MUL/MLA, long, and accumulate-long. */
    if (!check(UINT32_C(0x00000001), 1u, 1u, 2u) ||
        !check(UINT32_C(0x00000001), 0u, 2u, 3u) ||
        !check(UINT32_C(0x7FFFFFFF), 1u, 2u, 6u)) {
        return fail("multiply accumulate extra-cycle term was incorrect");
    }

    /* ARM9 uses the static internal-cycle accumulator, not this ARM7 helper. */
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 reselection failed");
    }
    if (runtime_mul_cycles(UINT32_C(0xFFFFFFFF), 1u, 2u) != 0u) {
        return fail("ARM9 must not add the ARM7 operand-dependent multiply term");
    }

    if (!oracis_arm_runtime_unbind_cpus(&arm9, &arm7)) {
        return fail("CPU unbinding failed");
    }

    std::puts("ARM7TDMI early-termination multiply timing tests passed");
    return 0;
}
