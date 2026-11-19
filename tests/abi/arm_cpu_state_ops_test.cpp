#include "oracis/recomp/arm_cpu_state_ops.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "arm_cpu_state_ops_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    ArmCpuState state{};
    state.cpsr = ORACIS_ARM_MODE_SYSTEM | CPSR_I_BIT | CPSR_F_BIT |
                 CPSR_N_BIT | CPSR_C_BIT;
    for (uint32_t index = 0; index < 5u; ++index) {
        state.R[8u + index] = UINT32_C(0x10000000) + index;
    }
    state.R[13] = UINT32_C(0x0207FFFC);
    state.R[14] = UINT32_C(0x02001000);
    state.banked_spsr[ARM_BANK_FIQ] = UINT32_C(0xA5A5A5A5);
    state.banked_spsr[ARM_BANK_IRQ] = UINT32_C(0x5A5A5A5A);

    const uint32_t preserved_cpsr_bits =
        state.cpsr & ~ORACIS_ARM_MODE_MASK;

    struct ModeCase {
        uint32_t mode;
        uint32_t expected_bank;
    };
    const ModeCase valid_modes[] = {
        {ORACIS_ARM_MODE_USER, ARM_BANK_USER},
        {ORACIS_ARM_MODE_SYSTEM, ARM_BANK_USER},
        {ORACIS_ARM_MODE_FIQ, ARM_BANK_FIQ},
        {ORACIS_ARM_MODE_IRQ, ARM_BANK_IRQ},
        {ORACIS_ARM_MODE_SUPERVISOR, ARM_BANK_SUPERVISOR},
        {ORACIS_ARM_MODE_ABORT, ARM_BANK_ABORT},
        {ORACIS_ARM_MODE_UNDEFINED, ARM_BANK_UNDEFINED},
    };
    uint32_t bank = UINT32_C(0xFFFFFFFF);
    for (const ModeCase &mode_case : valid_modes) {
        if (!oracis_arm_cpu_mode_bank(mode_case.mode, &bank) ||
            bank != mode_case.expected_bank) {
            return fail("valid ARM mode mapped to an incorrect bank");
        }
    }
    if (oracis_arm_cpu_mode_bank(UINT32_C(0x16), &bank) ||
        bank != ARM_BANK_UNDEFINED) {
        return fail("invalid mode must fail without overwriting bank_out");
    }
    if (oracis_arm_cpu_mode_bank(ORACIS_ARM_MODE_IRQ | CPSR_T_BIT, &bank)) {
        return fail("mode mapper must reject non-mode bits");
    }

    if (!oracis_arm_cpu_switch_mode(&state, ORACIS_ARM_MODE_FIQ)) {
        return fail("System -> FIQ transition failed");
    }
    if (state.banked_sp[ARM_BANK_USER] != UINT32_C(0x0207FFFC) ||
        state.banked_lr[ARM_BANK_USER] != UINT32_C(0x02001000)) {
        return fail("User/System SP/LR were not saved");
    }
    for (uint32_t index = 0; index < 5u; ++index) {
        if (state.r8_12_user[index] != UINT32_C(0x10000000) + index ||
            state.R[8u + index] != 0u) {
            return fail("FIQ R8-R12 bank was not exchanged");
        }
        state.R[8u + index] = UINT32_C(0xF1000000) + index;
    }
    state.R[13] = UINT32_C(0xF13FFFFC);
    state.R[14] = UINT32_C(0xF1001000);

    if (!oracis_arm_cpu_switch_mode(&state, ORACIS_ARM_MODE_IRQ)) {
        return fail("FIQ -> IRQ transition failed");
    }
    if (state.banked_sp[ARM_BANK_FIQ] != UINT32_C(0xF13FFFFC) ||
        state.banked_lr[ARM_BANK_FIQ] != UINT32_C(0xF1001000)) {
        return fail("FIQ SP/LR were not saved");
    }
    for (uint32_t index = 0; index < 5u; ++index) {
        if (state.r8_12_fiq[index] != UINT32_C(0xF1000000) + index ||
            state.R[8u + index] != UINT32_C(0x10000000) + index) {
            return fail("IRQ did not restore shared User R8-R12");
        }
        state.R[8u + index] = UINT32_C(0x20000000) + index;
    }

    state.R[13] = UINT32_C(0x03001000);
    state.R[14] = UINT32_C(0x03002000);
    if (!oracis_arm_cpu_switch_mode(&state, ORACIS_ARM_MODE_SYSTEM)) {
        return fail("IRQ -> System transition failed");
    }
    if (state.banked_sp[ARM_BANK_IRQ] != UINT32_C(0x03001000) ||
        state.banked_lr[ARM_BANK_IRQ] != UINT32_C(0x03002000)) {
        return fail("IRQ SP/LR were not saved");
    }
    if (state.R[13] != UINT32_C(0x0207FFFC) ||
        state.R[14] != UINT32_C(0x02001000)) {
        return fail("System mode did not restore the shared User SP/LR");
    }
    for (uint32_t index = 0; index < 5u; ++index) {
        if (state.R[8u + index] != UINT32_C(0x20000000) + index) {
            return fail("System mode did not retain shared User R8-R12");
        }
    }
    if ((state.cpsr & ~ORACIS_ARM_MODE_MASK) != preserved_cpsr_bits) {
        return fail("mode switching changed non-mode CPSR bits");
    }

    if (!oracis_arm_cpu_switch_mode(&state, ORACIS_ARM_MODE_FIQ)) {
        return fail("System -> FIQ restore transition failed");
    }
    if (state.R[13] != UINT32_C(0xF13FFFFC) ||
        state.R[14] != UINT32_C(0xF1001000)) {
        return fail("FIQ SP/LR were not restored");
    }
    for (uint32_t index = 0; index < 5u; ++index) {
        if (state.R[8u + index] != UINT32_C(0xF1000000) + index) {
            return fail("FIQ R8-R12 were not restored");
        }
    }
    if (state.banked_spsr[ARM_BANK_FIQ] != UINT32_C(0xA5A5A5A5) ||
        state.banked_spsr[ARM_BANK_IRQ] != UINT32_C(0x5A5A5A5A)) {
        return fail("mode switching modified banked SPSR storage");
    }

    ArmCpuState before_invalid = state;
    if (oracis_arm_cpu_switch_mode(&state, UINT32_C(0x16)) ||
        std::memcmp(&state, &before_invalid, sizeof(state)) != 0) {
        return fail("invalid mode must not modify CPU state");
    }
    if (oracis_arm_cpu_switch_mode(nullptr, ORACIS_ARM_MODE_USER)) {
        return fail("null CPU state must be rejected");
    }

    std::puts("ARM mode banking and register restoration tests passed");
    return 0;
}
