#include "oracis/recomp/runtime_arm.h"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "runtime_code_cycles_test: %s\n", message);
    return 1;
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

    /* No active ITCM before the machine publishes a valid CP15 window. */
    oracis_runtime_set_arm9_itcm_code_window(0u);
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 selection failed");
    }

    if (runtime_code_cycles(UINT32_C(0x02000000)) != 18u ||
        runtime_code_cycles(UINT32_C(0x03000000)) != 8u ||
        runtime_code_cycles(UINT32_C(0x04000200)) != 8u ||
        runtime_code_cycles(UINT32_C(0x05000000)) != 10u ||
        runtime_code_cycles(UINT32_C(0x06000000)) != 10u ||
        runtime_code_cycles(UINT32_C(0x07000000)) != 8u ||
        runtime_code_cycles(UINT32_C(0xFFFF0000)) != 8u) {
        return fail("ARM9 ARM-state fetch costs did not match the supported regions");
    }

    arm9.cpsr |= CPSR_T_BIT;
    if (runtime_code_cycles(UINT32_C(0x02000000)) != 18u ||
        runtime_code_cycles(UINT32_C(0x02000002)) != 0u ||
        runtime_code_cycles(UINT32_C(0x03000000)) != 8u ||
        runtime_code_cycles(UINT32_C(0x05000000)) != 10u ||
        runtime_code_cycles(UINT32_C(0x06000000)) != 10u ||
        runtime_code_cycles(UINT32_C(0x07000000)) != 8u) {
        return fail("ARM9 Thumb double-fetch timing was not split by halfword");
    }

    /* The ITCM region aperture can be smaller than its 32KB physical store. */
    oracis_runtime_set_arm9_itcm_code_window(UINT32_C(0x00001000));
    arm9.cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (runtime_code_cycles(UINT32_C(0x00000800)) != 1u) {
        return fail("4KB ITCM aperture was not accepted by the timing provider");
    }

    if (arm9_refill_cycles(UINT32_C(0x02000000)) != 36u ||
        arm9_refill_cycles(UINT32_C(0x02000001)) != 18u ||
        arm9_refill_cycles(UINT32_C(0x02000003)) != 36u ||
        arm9_refill_cycles(UINT32_C(0x03000000)) != 16u ||
        arm9_refill_cycles(UINT32_C(0x03000001)) != 8u ||
        arm9_refill_cycles(UINT32_C(0x05000000)) != 20u ||
        arm9_refill_cycles(UINT32_C(0x05000001)) != 10u ||
        arm9_refill_cycles(UINT32_C(0x05000003)) != 20u ||
        arm9_refill_cycles(UINT32_C(0xFFFF0000)) != 16u ||
        arm9_refill_cycles(UINT32_C(0x02FFFFFC)) != 26u) {
        return fail("ARM9 branch refill did not sum sequential fetches by target region");
    }

    /* The 32MB firmware ITCM mirror also covers the 0x01FF8000 boot window. */
    oracis_runtime_set_arm9_itcm_code_window(UINT32_C(0x02000000));
    arm9.cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (runtime_code_cycles(UINT32_C(0x01FF8000)) != 1u ||
        arm9_refill_cycles(UINT32_C(0x01FF8000)) != 2u) {
        return fail("ARM9 code fetch/refill in the active ITCM mirror was incorrect");
    }
    arm9.cpsr |= CPSR_T_BIT;
    if (runtime_code_cycles(UINT32_C(0x01FF8000)) != 1u ||
        runtime_code_cycles(UINT32_C(0x01FF8002)) != 0u ||
        arm9_refill_cycles(UINT32_C(0x01FF8001)) != 1u ||
        arm9_refill_cycles(UINT32_C(0x01FF8003)) != 2u) {
        return fail("ITCM Thumb fetch/refill timing did not follow word alignment");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM7)) {
        return fail("ARM7 selection failed");
    }
    if (runtime_code_cycles(UINT32_C(0x02380000)) != 0u ||
        arm7_refill_cycles(UINT32_C(0x00000000)) != 2u ||
        arm7_refill_cycles(UINT32_C(0x02000000)) != 4u ||
        arm7_refill_cycles(UINT32_C(0x02000001)) != 2u ||
        arm7_refill_cycles(UINT32_C(0x03000000)) != 2u ||
        arm7_refill_cycles(UINT32_C(0x04000200)) != 2u ||
        arm7_refill_cycles(UINT32_C(0x05000000)) != 4u ||
        arm7_refill_cycles(UINT32_C(0x05000001)) != 2u ||
        arm7_refill_cycles(UINT32_C(0x06000000)) != 4u ||
        arm7_refill_cycles(UINT32_C(0x06000001)) != 2u ||
        arm7_refill_cycles(UINT32_C(0x07000000)) != 2u ||
        arm7_refill_cycles(UINT32_C(0x02FFFFFC)) != 3u) {
        return fail("ARM7 code/refill costs did not match sequential target fetches");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 reselection failed");
    }
    oracis_runtime_set_arm9_itcm_code_window(0u);
    if (!oracis_arm_runtime_unbind_cpus(&arm9, &arm7)) {
        return fail("CPU unbinding failed");
    }

    std::puts("ARM9 uncached code-fetch timing tests passed");
    return 0;
}
