#include "oracis/recomp/arm_cpu_state_ops.h"

namespace {

bool mode_has_spsr(uint32_t mode)
{
    return mode != ORACIS_ARM_MODE_USER && mode != ORACIS_ARM_MODE_SYSTEM;
}

uint32_t msr_field_bits(uint32_t field_mask)
{
    uint32_t bit_mask = 0u;
    if ((field_mask & 0x1u) != 0u) bit_mask |= UINT32_C(0x000000FF);
    if ((field_mask & 0x2u) != 0u) bit_mask |= UINT32_C(0x0000FF00);
    if ((field_mask & 0x4u) != 0u) bit_mask |= UINT32_C(0x00FF0000);
    if ((field_mask & 0x8u) != 0u) bit_mask |= UINT32_C(0xFF000000);
    return bit_mask;
}

bool current_spsr_bank(const ArmCpuState *state, uint32_t &bank)
{
    if (state == nullptr) return false;
    const uint32_t mode = state->cpsr & ORACIS_ARM_MODE_MASK;
    if (!mode_has_spsr(mode)) return false;
    return oracis_arm_cpu_mode_bank(mode, &bank) != 0;
}

} // namespace

extern "C" int oracis_arm_cpu_read_spsr(const ArmCpuState *state,
                                          uint32_t *value)
{
    if (value == nullptr) return 0;
    uint32_t bank = 0u;
    if (!current_spsr_bank(state, bank)) return 0;
    *value = state->banked_spsr[bank];
    return 1;
}

extern "C" int oracis_arm_cpu_write_spsr(ArmCpuState *state,
                                           uint32_t value,
                                           uint32_t field_mask)
{
    if ((field_mask & ~UINT32_C(0xF)) != 0u) return 0;
    uint32_t bank = 0u;
    if (!current_spsr_bank(state, bank)) return 0;

    const uint32_t bit_mask = msr_field_bits(field_mask);
    const uint32_t old_value = state->banked_spsr[bank];
    state->banked_spsr[bank] = (old_value & ~bit_mask) | (value & bit_mask);
    return 1;
}

extern "C" int oracis_arm_cpu_write_cpsr(ArmCpuState *state,
                                           uint32_t value,
                                           uint32_t field_mask)
{
    if (state == nullptr || (field_mask & ~UINT32_C(0xF)) != 0u) return 0;

    uint32_t bit_mask = msr_field_bits(field_mask);
    const uint32_t old_mode = state->cpsr & ORACIS_ARM_MODE_MASK;

    /* Unprivileged User mode may update the flags field only. */
    if (old_mode == ORACIS_ARM_MODE_USER) {
        bit_mask &= UINT32_C(0xFF000000);
    }

    const uint32_t new_cpsr =
        (state->cpsr & ~bit_mask) | (value & bit_mask);
    const uint32_t new_mode = new_cpsr & ORACIS_ARM_MODE_MASK;

    /* Validate the requested mode before changing any visible registers. */
    uint32_t ignored_bank = 0u;
    if (!oracis_arm_cpu_mode_bank(new_mode, &ignored_bank)) return 0;

    if (new_mode != old_mode &&
        !oracis_arm_cpu_switch_mode(state, new_mode)) {
        return 0;
    }

    state->cpsr = new_cpsr;
    return 1;
}

extern "C" int oracis_arm_cpu_restore_cpsr_from_spsr(ArmCpuState *state)
{
    uint32_t saved_cpsr = 0u;
    if (!oracis_arm_cpu_read_spsr(state, &saved_cpsr)) return 0;
    return oracis_arm_cpu_write_cpsr(state, saved_cpsr, UINT32_C(0xF));
}
