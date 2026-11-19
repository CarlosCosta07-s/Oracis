#include "oracis/recomp/arm_cpu_state_ops.h"

#include <stddef.h>

namespace {

bool mode_bank(uint32_t mode, uint32_t &bank)
{
    switch (mode) {
    case ORACIS_ARM_MODE_USER:
    case ORACIS_ARM_MODE_SYSTEM:
        bank = ARM_BANK_USER;
        return true;
    case ORACIS_ARM_MODE_FIQ:
        bank = ARM_BANK_FIQ;
        return true;
    case ORACIS_ARM_MODE_IRQ:
        bank = ARM_BANK_IRQ;
        return true;
    case ORACIS_ARM_MODE_SUPERVISOR:
        bank = ARM_BANK_SUPERVISOR;
        return true;
    case ORACIS_ARM_MODE_ABORT:
        bank = ARM_BANK_ABORT;
        return true;
    case ORACIS_ARM_MODE_UNDEFINED:
        bank = ARM_BANK_UNDEFINED;
        return true;
    default:
        return false;
    }
}

void save_visible_registers(ArmCpuState &state, uint32_t bank, bool fiq)
{
    state.banked_sp[bank] = state.R[13];
    state.banked_lr[bank] = state.R[14];

    uint32_t *destination = fiq ? state.r8_12_fiq : state.r8_12_user;
    for (size_t index = 0; index < 5u; ++index) {
        destination[index] = state.R[8u + index];
    }
}

void load_visible_registers(ArmCpuState &state, uint32_t bank, bool fiq)
{
    state.R[13] = state.banked_sp[bank];
    state.R[14] = state.banked_lr[bank];

    const uint32_t *source = fiq ? state.r8_12_fiq : state.r8_12_user;
    for (size_t index = 0; index < 5u; ++index) {
        state.R[8u + index] = source[index];
    }
}

} // namespace

extern "C" int oracis_arm_cpu_mode_bank(uint32_t mode, uint32_t *bank_out)
{
    if (bank_out == nullptr || (mode & ~ORACIS_ARM_MODE_MASK) != 0u) {
        return 0;
    }

    uint32_t bank = 0u;
    if (!mode_bank(mode, bank)) {
        return 0;
    }

    *bank_out = bank;
    return 1;
}

extern "C" int oracis_arm_cpu_switch_mode(ArmCpuState *state, uint32_t new_mode)
{
    if (state == nullptr || (new_mode & ~ORACIS_ARM_MODE_MASK) != 0u) {
        return 0;
    }

    const uint32_t old_mode = state->cpsr & ORACIS_ARM_MODE_MASK;
    uint32_t old_bank = 0u;
    uint32_t new_bank = 0u;

    if (!mode_bank(old_mode, old_bank) || !mode_bank(new_mode, new_bank)) {
        return 0;
    }

    if (old_mode == new_mode) {
        return 1;
    }

    const bool old_fiq = old_mode == ORACIS_ARM_MODE_FIQ;
    const bool new_fiq = new_mode == ORACIS_ARM_MODE_FIQ;

    save_visible_registers(*state, old_bank, old_fiq);
    load_visible_registers(*state, new_bank, new_fiq);

    state->cpsr = (state->cpsr & ~ORACIS_ARM_MODE_MASK) | new_mode;
    return 1;
}
