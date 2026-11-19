/*
 * Oracis ARM CPU mode/banked-register operations.
 * The implementation is independent of the NDSRecomp runner.
 */
#ifndef ORACIS_RECOMP_ARM_CPU_STATE_OPS_H
#define ORACIS_RECOMP_ARM_CPU_STATE_OPS_H

#include "oracis/recomp/arm_cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ORACIS_ARM_MODE_MASK UINT32_C(0x1F)

#define ORACIS_ARM_MODE_USER       UINT32_C(0x10)
#define ORACIS_ARM_MODE_FIQ        UINT32_C(0x11)
#define ORACIS_ARM_MODE_IRQ        UINT32_C(0x12)
#define ORACIS_ARM_MODE_SUPERVISOR UINT32_C(0x13)
#define ORACIS_ARM_MODE_ABORT      UINT32_C(0x17)
#define ORACIS_ARM_MODE_UNDEFINED  UINT32_C(0x1B)
#define ORACIS_ARM_MODE_SYSTEM     UINT32_C(0x1F)

/*
 * Return 1 and write the bank index for a valid ARM mode; otherwise return 0
 * and leave bank_out untouched. The input must contain only the five mode bits.
 */
int oracis_arm_cpu_mode_bank(uint32_t mode, uint32_t *bank_out);

/*
 * Switch the CPSR mode field and exchange the visible banked registers.
 * new_mode must be one of ORACIS_ARM_MODE_*. All non-mode CPSR bits and all
 * SPSR banks are preserved. Returns 0 without modifying state on invalid input.
 */
int oracis_arm_cpu_switch_mode(ArmCpuState *state, uint32_t new_mode);

/* PSR operations. field_mask uses the ARM MSR c/x/s/f field bits. */
int oracis_arm_cpu_read_spsr(const ArmCpuState *state, uint32_t *value);
int oracis_arm_cpu_write_spsr(ArmCpuState *state, uint32_t value, uint32_t field_mask);
int oracis_arm_cpu_write_cpsr(ArmCpuState *state, uint32_t value, uint32_t field_mask);
int oracis_arm_cpu_restore_cpsr_from_spsr(ArmCpuState *state);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ORACIS_RECOMP_ARM_CPU_STATE_OPS_H */
