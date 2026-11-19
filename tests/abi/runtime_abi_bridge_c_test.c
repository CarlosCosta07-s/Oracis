#include "oracis/recomp/runtime_arm.h"

#include <stdint.h>
#include <stdio.h>

int main(void)
{
    ArmCpuState arm9 = {0};
    ArmCpuState arm7 = {0};

    arm9.cpsr = UINT32_C(0x1F);
    arm7.cpsr = UINT32_C(0x1F);

    if (!oracis_arm_runtime_bind_cpus(&arm9, &arm7)) {
        fputs("C ABI bridge: failed to bind CPU states\n", stderr);
        return 1;
    }

    g_cpu.R[0] = UINT32_C(0x12345678);
    arm_set_nz(UINT32_C(0x80000000));
    if (arm9.R[0] != UINT32_C(0x12345678) ||
        (arm9.cpsr & CPSR_N_BIT) == 0u ||
        (arm7.cpsr & CPSR_N_BIT) != 0u) {
        fputs("C ABI bridge: ARM9 state alias failed\n", stderr);
        return 1;
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM7)) {
        fputs("C ABI bridge: failed to select ARM7\n", stderr);
        return 1;
    }

    g_cpu.R[1] = UINT32_C(0x87654321);
    arm_set_nzcv_add(UINT32_C(0x7FFFFFFF), 1u, UINT32_C(0x80000000));
    if (arm7.R[1] != UINT32_C(0x87654321) ||
        (arm7.cpsr & (CPSR_N_BIT | CPSR_V_BIT)) !=
            (CPSR_N_BIT | CPSR_V_BIT) ||
        (arm9.cpsr & CPSR_V_BIT) != 0u) {
        fputs("C ABI bridge: ARM7 state alias failed\n", stderr);
        return 1;
    }

    if (!arm_cond_passes_i(0xAu) ||
        !arm_cond_passes(0xAu)) {
        fputs("C ABI bridge: condition helper failed\n", stderr);
        return 1;
    }

    puts("C99 generated-bank header/bridge test passed");
    return 0;
}
