#include "oracis/recomp/arm_cpu_state_ops.h"

#include <limits.h>

namespace {

struct ExceptionProfile {
    uint32_t mode;
    uint32_t vector_offset;
    bool mask_fiq;
};

bool exception_profile(OracisArmException exception, ExceptionProfile &profile)
{
    switch (exception) {
    case ORACIS_ARM_EXCEPTION_UNDEFINED:
        profile = {ORACIS_ARM_MODE_UNDEFINED, UINT32_C(0x04), false};
        return true;
    case ORACIS_ARM_EXCEPTION_SWI:
        profile = {ORACIS_ARM_MODE_SUPERVISOR, UINT32_C(0x08), false};
        return true;
    case ORACIS_ARM_EXCEPTION_PREFETCH_ABORT:
        profile = {ORACIS_ARM_MODE_ABORT, UINT32_C(0x0C), false};
        return true;
    case ORACIS_ARM_EXCEPTION_DATA_ABORT:
        profile = {ORACIS_ARM_MODE_ABORT, UINT32_C(0x10), false};
        return true;
    case ORACIS_ARM_EXCEPTION_IRQ:
        profile = {ORACIS_ARM_MODE_IRQ, UINT32_C(0x18), false};
        return true;
    case ORACIS_ARM_EXCEPTION_FIQ:
        profile = {ORACIS_ARM_MODE_FIQ, UINT32_C(0x1C), true};
        return true;
    default:
        return false;
    }
}

} // namespace

extern "C" int oracis_arm_cpu_enter_exception(
    ArmCpuState *state,
    OracisArmException exception,
    uint32_t return_address,
    uint32_t vector_base)
{
    if (state == nullptr) return 0;

    ExceptionProfile profile{};
    if (!exception_profile(exception, profile)) return 0;

    if (vector_base > UINT32_MAX - profile.vector_offset) return 0;

    const uint32_t saved_cpsr = state->cpsr;
    const uint32_t old_mode = saved_cpsr & ORACIS_ARM_MODE_MASK;
    uint32_t old_bank = 0u;
    uint32_t exception_bank = 0u;

    /* Refuse to enter from a corrupted/unsupported current processor mode. */
    if (!oracis_arm_cpu_mode_bank(old_mode, &old_bank) ||
        !oracis_arm_cpu_mode_bank(profile.mode, &exception_bank)) {
        return 0;
    }

    if (!oracis_arm_cpu_switch_mode(state, profile.mode)) return 0;

    /* Exception mode owns its SPSR/LR bank. The original CPSR is captured
       before changing mode or interrupt masks. */
    state->banked_spsr[exception_bank] = saved_cpsr;
    state->R[14] = return_address;

    uint32_t next_cpsr = (saved_cpsr & ~ORACIS_ARM_MODE_MASK);
    next_cpsr &= ~CPSR_T_BIT;
    next_cpsr |= profile.mode | CPSR_I_BIT;
    if (profile.mask_fiq) next_cpsr |= CPSR_F_BIT;

    state->cpsr = next_cpsr;
    state->R[15] = vector_base + profile.vector_offset;
    return 1;
}

extern "C" int oracis_arm_cpu_return_from_exception(
    ArmCpuState *state,
    uint32_t return_address)
{
    if (state == nullptr) return 0;

    uint32_t saved_cpsr = 0u;
    if (!oracis_arm_cpu_read_spsr(state, &saved_cpsr)) return 0;

    uint32_t restored_bank = 0u;
    if (!oracis_arm_cpu_mode_bank(
            saved_cpsr & ORACIS_ARM_MODE_MASK, &restored_bank)) {
        return 0;
    }

    const uint32_t alignment = (saved_cpsr & CPSR_T_BIT) != 0u ? 2u : 4u;
    if ((return_address % alignment) != 0u) return 0;

    if (!oracis_arm_cpu_restore_cpsr_from_spsr(state)) return 0;

    /* Publish PC only after CPSR and banked registers have been restored. */
    state->R[15] = return_address;
    return 1;
}
