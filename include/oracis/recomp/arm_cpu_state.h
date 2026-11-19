/*
 * Oracis ARM CPU state ABI
 *
 * This is an independently maintained, C-compatible POD contract for
 * NDSRecomp-generated code. It defines storage only; execution semantics live
 * in the Oracis runtime and are not copied from the NDSRecomp runner.
 */

#ifndef ORACIS_RECOMP_ARM_CPU_STATE_H
#define ORACIS_RECOMP_ARM_CPU_STATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NdsCpu {
    NDS_ARM9 = 0,
    NDS_ARM7 = 1
} NdsCpu;

/* Bank indexes used by ARM7/ARM9 privileged register state. */
#define ARM_BANK_USER       0u
#define ARM_BANK_FIQ        1u
#define ARM_BANK_IRQ        2u
#define ARM_BANK_SUPERVISOR 3u
#define ARM_BANK_ABORT      4u
#define ARM_BANK_UNDEFINED  5u
#define ARM_BANK_COUNT      6u

/*
 * Architectural register storage exposed to generated C functions.
 *
 * Keep this layout stable: generated bodies access R[] and cpsr directly,
 * while the runtime owns bank switching and exception semantics.
 */
typedef struct ArmCpuState {
    uint32_t R[16];
    uint32_t cpsr;

    uint32_t banked_sp[ARM_BANK_COUNT];
    uint32_t banked_lr[ARM_BANK_COUNT];
    uint32_t banked_spsr[ARM_BANK_COUNT];

    uint32_t r8_12_user[5];
    uint32_t r8_12_fiq[5];
} ArmCpuState;

/* CPSR bit positions from the ARM architecture. */
#define CPSR_N_BIT (UINT32_C(1) << 31)
#define CPSR_Z_BIT (UINT32_C(1) << 30)
#define CPSR_C_BIT (UINT32_C(1) << 29)
#define CPSR_V_BIT (UINT32_C(1) << 28)
#define CPSR_Q_BIT (UINT32_C(1) << 27)
#define CPSR_I_BIT (UINT32_C(1) <<  7)
#define CPSR_F_BIT (UINT32_C(1) <<  6)
#define CPSR_T_BIT (UINT32_C(1) <<  5)

/* Fixed-width ABI checks that work in both C99 and C++. */
#define ORACIS_ABI_ASSERT(name, expression) typedef char name[(expression) ? 1 : -1]
ORACIS_ABI_ASSERT(oracis_arm_cpu_r_offset, offsetof(ArmCpuState, R) == 0u);
ORACIS_ABI_ASSERT(oracis_arm_cpu_cpsr_offset, offsetof(ArmCpuState, cpsr) == 64u);
ORACIS_ABI_ASSERT(oracis_arm_cpu_banked_sp_offset, offsetof(ArmCpuState, banked_sp) == 68u);
ORACIS_ABI_ASSERT(oracis_arm_cpu_banked_lr_offset, offsetof(ArmCpuState, banked_lr) == 92u);
ORACIS_ABI_ASSERT(oracis_arm_cpu_banked_spsr_offset, offsetof(ArmCpuState, banked_spsr) == 116u);
ORACIS_ABI_ASSERT(oracis_arm_cpu_user_regs_offset, offsetof(ArmCpuState, r8_12_user) == 140u);
ORACIS_ABI_ASSERT(oracis_arm_cpu_fiq_regs_offset, offsetof(ArmCpuState, r8_12_fiq) == 160u);
ORACIS_ABI_ASSERT(oracis_arm_cpu_state_size, sizeof(ArmCpuState) == 180u);
#undef ORACIS_ABI_ASSERT

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ORACIS_RECOMP_ARM_CPU_STATE_H */
