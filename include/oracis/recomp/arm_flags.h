/*
 * Oracis ARM condition-code and arithmetic flag helpers.
 * These operations work on an explicit CPU state; they do not use a global
 * active-CPU pointer or import the NDSRecomp runtime.
 */
#ifndef ORACIS_RECOMP_ARM_FLAGS_H
#define ORACIS_RECOMP_ARM_FLAGS_H

#include "oracis/recomp/arm_cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Evaluate an ARM condition field (0..15) against CPSR N/Z/C/V.
 * Invalid condition values are rejected. ARMv4T/ARMv5TE NV (0xF) is false.
 */
int oracis_arm_condition_passes(uint32_t cpsr, uint32_t condition);

void oracis_arm_set_nz(ArmCpuState *state, uint32_t result);
void oracis_arm_set_nzc_logic(
    ArmCpuState *state, uint32_t result, uint32_t shifter_carry);
void oracis_arm_set_nzcv_add(
    ArmCpuState *state, uint32_t a, uint32_t b, uint32_t result);
void oracis_arm_set_nzcv_adc(
    ArmCpuState *state, uint32_t a, uint32_t b,
    uint32_t carry_in, uint32_t result);
void oracis_arm_set_nzcv_sub(
    ArmCpuState *state, uint32_t a, uint32_t b, uint32_t result);
void oracis_arm_set_nzcv_sbc(
    ArmCpuState *state, uint32_t a, uint32_t b,
    uint32_t carry_in, uint32_t result);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ORACIS_RECOMP_ARM_FLAGS_H */
