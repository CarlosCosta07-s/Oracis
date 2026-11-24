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

    /* No active TCM before the machine publishes a valid CP15 window. */
    oracis_runtime_set_arm9_tcm_timing_state(0u, 0u, 0u, 0u, 0u);
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

    /* NDS9/DATA bus-cycle values are converted to 66MHz ARM9 core cycles. */
    if (runtime_mem_cycles(UINT32_C(0x02000000), 4u, 0u) != 20u ||
        runtime_mem_cycles(UINT32_C(0x02000000), 4u, 1u) != 4u ||
        runtime_mem_cycles(UINT32_C(0x02000000), 2u, 0u) != 18u ||
        runtime_mem_cycles(UINT32_C(0x02000000), 2u, 1u) != 2u ||
        runtime_mem_cycles(UINT32_C(0x03000000), 4u, 0u) != 8u ||
        runtime_mem_cycles(UINT32_C(0x03000000), 4u, 1u) != 2u ||
        runtime_mem_cycles(UINT32_C(0x04000000), 2u, 0u) != 8u ||
        runtime_mem_cycles(UINT32_C(0x04000000), 2u, 1u) != 2u ||
        runtime_mem_cycles(UINT32_C(0x07000000), 4u, 0u) != 8u ||
        runtime_mem_cycles(UINT32_C(0x05000000), 4u, 0u) != 10u ||
        runtime_mem_cycles(UINT32_C(0x05000000), 4u, 1u) != 4u ||
        runtime_mem_cycles(UINT32_C(0x06000000), 2u, 0u) != 8u ||
        runtime_mem_cycles(UINT32_C(0x06000000), 2u, 1u) != 2u ||
        runtime_mem_cycles(UINT32_C(0xFFFF0000), 1u, 0u) != 8u) {
        return fail("ARM9 data access timings did not match NDS9/DATA classes");
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

    if (runtime_mem_cycles(UINT32_C(0x02000000), 2u, 1u) != 2u ||
        runtime_mem_cycles(UINT32_C(0x05000000), 2u, 0u) != 8u) {
        return fail("ARM9 Thumb state changed data-bus wait-state costs");
    }

    /* The ITCM region aperture can be smaller than its 32KB physical store. */
    oracis_runtime_set_arm9_tcm_timing_state(
        UINT32_C(0x00001000), 0u, 0u, 0u, 0u);
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
    oracis_runtime_set_arm9_tcm_timing_state(
        UINT32_C(0x02000000), UINT32_C(0x027C0000),
        UINT32_C(0x00004000), 0u, 0u);
    arm9.cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (runtime_code_cycles(UINT32_C(0x01FF8000)) != 1u ||
        runtime_mem_cycles(UINT32_C(0x01FF8000), 4u, 0u) != 1u ||
        runtime_mem_cycles(UINT32_C(0x027C0000), 4u, 0u) != 1u ||
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

    /*
     * The direction-aware helper models ARM946E-S load mode: DATA reads
     * bypass the TCM overlay and access the underlying external region,
     * while writes still fill the private TCM backing at one core cycle.
     */
    if (runtime_mem_cycles_ex(
            UINT32_C(0x01FF8000), 4u, 0u,
            ORACIS_MEMORY_ACCESS_READ) != 1u ||
        runtime_mem_cycles_ex(
            UINT32_C(0x01FF8000), 4u, 0u,
            ORACIS_MEMORY_ACCESS_WRITE) != 1u ||
        runtime_mem_cycles_ex(
            UINT32_C(0x027C0000), 4u, 0u,
            ORACIS_MEMORY_ACCESS_READ) != 1u ||
        runtime_mem_cycles_ex(
            UINT32_C(0x027C0000), 4u, 0u,
            ORACIS_MEMORY_ACCESS_WRITE) != 1u) {
        return fail("normal ITCM/DTCM direction-aware timing changed");
    }

    oracis_runtime_set_arm9_tcm_timing_state(
        UINT32_C(0x02000000), UINT32_C(0x027C0000),
        UINT32_C(0x00004000), 1u, 1u);
    if (runtime_mem_cycles_ex(
            UINT32_C(0x027C0000), 4u, 0u,
            ORACIS_MEMORY_ACCESS_WRITE) != 1u ||
        runtime_mem_cycles_ex(
            UINT32_C(0x027C0000), 4u, 0u,
            ORACIS_MEMORY_ACCESS_READ) != 20u ||
        runtime_mem_cycles_ex(
            UINT32_C(0x027C0000), 4u, 1u,
            ORACIS_MEMORY_ACCESS_READ) != 4u ||
        runtime_mem_cycles_ex(
            UINT32_C(0x027C0000), 2u, 0u,
            ORACIS_MEMORY_ACCESS_READ) != 18u ||
        runtime_mem_cycles_ex(
            UINT32_C(0x027C3FFE), 4u, 0u,
            ORACIS_MEMORY_ACCESS_READ) != 20u ||
        runtime_mem_cycles_ex(
            UINT32_C(0x00001000), 4u, 0u,
            ORACIS_MEMORY_ACCESS_WRITE) != 1u) {
        return fail("ITCM/DTCM load-mode read/write timing was not distinguished");
    }

    /* Restore normal mode before validating ARM7 and unbinding the runtime. */
    oracis_runtime_set_arm9_tcm_timing_state(
        UINT32_C(0x02000000), UINT32_C(0x027C0000),
        UINT32_C(0x00004000), 0u, 0u);

    if (!oracis_arm_runtime_select_cpu(NDS_ARM7)) {
        return fail("ARM7 selection failed");
    }
    /* ARM7 may use VRAM C/D as work RAM at both 128KB bank offsets. */
    if (runtime_code_cycles(UINT32_C(0x02380000)) != 0u ||
        runtime_mem_cycles(UINT32_C(0x02000000), 4u, 0u) != 10u ||
        runtime_mem_cycles(UINT32_C(0x02000000), 4u, 1u) != 2u ||
        runtime_mem_cycles(UINT32_C(0x02000000), 2u, 0u) != 9u ||
        runtime_mem_cycles(UINT32_C(0x02000000), 2u, 1u) != 1u ||
        runtime_mem_cycles(UINT32_C(0x03000000), 4u, 0u) != 1u ||
        runtime_mem_cycles(UINT32_C(0x00000000), 2u, 0u) != 1u ||
        runtime_mem_cycles(UINT32_C(0x06000000), 4u, 0u) != 1u ||
        runtime_mem_cycles(UINT32_C(0x06000000), 4u, 1u) != 2u ||
        runtime_mem_cycles(UINT32_C(0x06000000), 2u, 0u) != 1u ||
        runtime_mem_cycles(UINT32_C(0x06020000), 4u, 0u) != 1u ||
        runtime_mem_cycles(UINT32_C(0x06020000), 4u, 1u) != 2u ||
        runtime_mem_cycles(UINT32_C(0x06020000), 2u, 0u) != 1u ||
        arm7_refill_cycles(UINT32_C(0x00000000)) != 2u ||
        arm7_refill_cycles(UINT32_C(0x02000000)) != 4u ||
        arm7_refill_cycles(UINT32_C(0x02000001)) != 2u ||
        arm7_refill_cycles(UINT32_C(0x03000000)) != 2u ||
        arm7_refill_cycles(UINT32_C(0x04000200)) != 2u ||
        arm7_refill_cycles(UINT32_C(0x06020000)) != 4u ||
        arm7_refill_cycles(UINT32_C(0x06020001)) != 2u ||
        arm7_refill_cycles(UINT32_C(0x06000000)) != 4u ||
        arm7_refill_cycles(UINT32_C(0x06000001)) != 2u ||
        arm7_refill_cycles(UINT32_C(0x07000000)) != 2u ||
        arm7_refill_cycles(UINT32_C(0x02FFFFFC)) != 3u) {
        return fail("ARM7 code/refill costs did not match sequential target fetches");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 reselection failed");
    }
    oracis_runtime_set_arm9_tcm_timing_state(0u, 0u, 0u, 0u, 0u);
    if (!oracis_arm_runtime_unbind_cpus(&arm9, &arm7)) {
        return fail("CPU unbinding failed");
    }

    std::puts("ARM9 uncached code-fetch timing tests passed");
    return 0;
}
