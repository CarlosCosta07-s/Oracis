#include "oracis/recomp/arm_cpu_state_ops.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "arm_psr_ops_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    ArmCpuState state{};
    state.cpsr = ORACIS_ARM_MODE_USER | CPSR_T_BIT | CPSR_I_BIT;
    state.R[13] = UINT32_C(0x0207FFFC);
    state.R[14] = UINT32_C(0x02001000);
    state.banked_spsr[ARM_BANK_IRQ] = UINT32_C(0xCAFEBABE);

    /* User mode may change the flags byte, but not control bits or mode. */
    if (!oracis_arm_cpu_write_cpsr(&state, CPSR_Z_BIT | CPSR_Q_BIT, 0x8u)) {
        return fail("User-mode flags write failed");
    }
    if ((state.cpsr & (CPSR_Z_BIT | CPSR_Q_BIT)) !=
        (CPSR_Z_BIT | CPSR_Q_BIT) ||
        (state.cpsr & (CPSR_T_BIT | CPSR_I_BIT)) !=
        (CPSR_T_BIT | CPSR_I_BIT)) {
        return fail("User-mode flags write changed unrelated CPSR fields");
    }
    if (!oracis_arm_cpu_write_cpsr(&state, ORACIS_ARM_MODE_IRQ, 0x1u) ||
        (state.cpsr & ORACIS_ARM_MODE_MASK) != ORACIS_ARM_MODE_USER) {
        return fail("User mode changed privileged CPSR control bits");
    }
    uint32_t spsr_value = 0u;
    if (oracis_arm_cpu_read_spsr(&state, &spsr_value) ||
        oracis_arm_cpu_write_spsr(&state, 0u, 0xFu)) {
        return fail("User mode must not access SPSR");
    }

    /* A privileged CPSR control write switches the visible SP/LR bank. */
    state.cpsr = ORACIS_ARM_MODE_SYSTEM | CPSR_I_BIT | CPSR_F_BIT;
    state.R[13] = UINT32_C(0x0207FFFC);
    state.R[14] = UINT32_C(0x02001000);
    state.banked_sp[ARM_BANK_IRQ] = UINT32_C(0x0300FFFC);
    state.banked_lr[ARM_BANK_IRQ] = UINT32_C(0x03001000);
    if (!oracis_arm_cpu_write_cpsr(
            &state, ORACIS_ARM_MODE_IRQ | CPSR_T_BIT | CPSR_I_BIT | CPSR_F_BIT, 0x1u)) {
        return fail("privileged System -> IRQ CPSR write failed");
    }
    if ((state.cpsr & ORACIS_ARM_MODE_MASK) != ORACIS_ARM_MODE_IRQ ||
        (state.cpsr & (CPSR_T_BIT | CPSR_I_BIT | CPSR_F_BIT)) !=
            (CPSR_T_BIT | CPSR_I_BIT | CPSR_F_BIT) ||
        state.R[13] != UINT32_C(0x0300FFFC) ||
        state.R[14] != UINT32_C(0x03001000) ||
        state.banked_sp[ARM_BANK_USER] != UINT32_C(0x0207FFFC)) {
        return fail("CPSR mode write did not switch the correct bank");
    }

    /* SPSR fields use c/x/s/f byte masks. */
    state.banked_spsr[ARM_BANK_IRQ] = UINT32_C(0xAABBCCDD);
    if (!oracis_arm_cpu_write_spsr(&state, UINT32_C(0x11223344), 0x5u)) {
        return fail("partial SPSR write failed");
    }
    if (!oracis_arm_cpu_read_spsr(&state, &spsr_value) ||
        spsr_value != UINT32_C(0xAA22CC44)) {
        return fail("partial SPSR field mask was applied incorrectly");
    }

    /* An invalid saved mode must make exception return fail without mutation. */
    if (!oracis_arm_cpu_write_spsr(&state, UINT32_C(0x00000016), 0x1u)) {
        return fail("SPSR invalid-mode fixture write failed");
    }
    ArmCpuState before_invalid_restore = state;
    if (oracis_arm_cpu_restore_cpsr_from_spsr(&state) ||
        std::memcmp(&state, &before_invalid_restore, sizeof(state)) != 0) {
        return fail("invalid SPSR mode must not mutate CPU state");
    }

    /* A valid SPSR restores the prior mode, T bit, and flags as one operation. */
    const uint32_t restored =
        ORACIS_ARM_MODE_SYSTEM | CPSR_T_BIT | CPSR_Z_BIT | CPSR_F_BIT;
    if (!oracis_arm_cpu_write_spsr(&state, restored, 0xFu) ||
        !oracis_arm_cpu_restore_cpsr_from_spsr(&state)) {
        return fail("valid CPSR restore from SPSR failed");
    }
    if (state.cpsr != restored ||
        state.R[13] != UINT32_C(0x0207FFFC) ||
        state.R[14] != UINT32_C(0x02001000) ||
        state.banked_sp[ARM_BANK_IRQ] != UINT32_C(0x0300FFFC)) {
        return fail("exception return did not restore System state");
    }
    if (oracis_arm_cpu_read_spsr(&state, &spsr_value)) {
        return fail("System mode must not expose an SPSR");
    }

    /* Invalid writes and invalid masks are fail-closed. */
    ArmCpuState before_invalid_write = state;
    if (oracis_arm_cpu_write_cpsr(&state, UINT32_C(0x16), 0x1u) ||
        std::memcmp(&state, &before_invalid_write, sizeof(state)) != 0) {
        return fail("invalid CPSR mode write must not mutate state");
    }
    if (oracis_arm_cpu_write_cpsr(&state, 0u, 0x10u) ||
        oracis_arm_cpu_write_spsr(&state, 0u, 0x10u)) {
        return fail("out-of-range MSR field mask must be rejected");
    }

    std::puts("CPSR/SPSR privilege, mask, and exception-return tests passed");
    return 0;
}
