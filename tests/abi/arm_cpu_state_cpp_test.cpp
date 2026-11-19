#include "oracis/recomp/arm_cpu_state.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

static_assert(sizeof(ArmCpuState) == 180u);
static_assert(offsetof(ArmCpuState, R) == 0u);
static_assert(offsetof(ArmCpuState, cpsr) == 64u);
static_assert(offsetof(ArmCpuState, banked_sp) == 68u);
static_assert(offsetof(ArmCpuState, banked_lr) == 92u);
static_assert(offsetof(ArmCpuState, banked_spsr) == 116u);
static_assert(offsetof(ArmCpuState, r8_12_user) == 140u);
static_assert(offsetof(ArmCpuState, r8_12_fiq) == 160u);

int main()
{
    ArmCpuState arm9{};
    ArmCpuState arm7{};

    arm9.R[15] = UINT32_C(0x02000800);
    arm7.R[15] = UINT32_C(0x02380000);
    arm9.cpsr = 0u;
    arm7.cpsr = CPSR_T_BIT;

    if (arm9.R[15] == arm7.R[15] ||
        (arm7.cpsr & CPSR_T_BIT) == 0u ||
        (arm9.cpsr & CPSR_T_BIT) != 0u) {
        std::fputs("ARM9/ARM7 state separation test failed\n", stderr);
        return 1;
    }

    std::puts("ARM CPU state C++ ABI test passed");
    return 0;
}
