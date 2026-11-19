#include "oracis/recomp/arm_cpu_state.h"

#include <stdio.h>

int main(void)
{
    ArmCpuState cpu = {0};

    cpu.R[0] = UINT32_C(0x11223344);
    cpu.R[13] = UINT32_C(0x0207FFFC);
    cpu.R[15] = UINT32_C(0x02000800);
    cpu.cpsr = CPSR_I_BIT | CPSR_F_BIT;

    if (cpu.R[0] != UINT32_C(0x11223344) ||
        cpu.R[13] != UINT32_C(0x0207FFFC) ||
        cpu.R[15] != UINT32_C(0x02000800) ||
        cpu.cpsr != (CPSR_I_BIT | CPSR_F_BIT)) {
        fputs("ARM CPU state storage test failed\n", stderr);
        return 1;
    }

    printf("ARM CPU state ABI: size=%u bytes, R15 offset=%u, CPSR offset=%u\n",
           (unsigned)sizeof(cpu),
           (unsigned)offsetof(ArmCpuState, R[15]),
           (unsigned)offsetof(ArmCpuState, cpsr));
    return 0;
}
