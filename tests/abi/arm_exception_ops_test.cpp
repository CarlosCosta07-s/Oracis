#include "oracis/recomp/arm_cpu_state_ops.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "arm_exception_ops_test: %s\n", message);
    return 1;
}

struct ExceptionCase {
    OracisArmException exception;
    uint32_t mode;
    uint32_t vector_offset;
    bool masks_fiq;
};

} // namespace

int main()
{
    const ExceptionCase cases[] = {
        {ORACIS_ARM_EXCEPTION_UNDEFINED, ORACIS_ARM_MODE_UNDEFINED, 0x04u, false},
        {ORACIS_ARM_EXCEPTION_SWI, ORACIS_ARM_MODE_SUPERVISOR, 0x08u, false},
        {ORACIS_ARM_EXCEPTION_PREFETCH_ABORT, ORACIS_ARM_MODE_ABORT, 0x0Cu, false},
        {ORACIS_ARM_EXCEPTION_DATA_ABORT, ORACIS_ARM_MODE_ABORT, 0x10u, false},
        {ORACIS_ARM_EXCEPTION_IRQ, ORACIS_ARM_MODE_IRQ, 0x18u, false},
        {ORACIS_ARM_EXCEPTION_FIQ, ORACIS_ARM_MODE_FIQ, 0x1Cu, true},
    };

    for (const ExceptionCase &entry : cases) {
        ArmCpuState state{};
        const uint32_t original_cpsr =
            ORACIS_ARM_MODE_SYSTEM | CPSR_T_BIT | CPSR_Z_BIT | CPSR_F_BIT;
        state.cpsr = original_cpsr;
        state.R[13] = UINT32_C(0x0207FFFC);
        state.R[14] = UINT32_C(0x02001000);
        state.R[15] = UINT32_C(0x02002000);
        state.banked_sp[ARM_BANK_IRQ] = UINT32_C(0x0300FFFC);
        state.banked_lr[ARM_BANK_IRQ] = UINT32_C(0x03001000);
        state.banked_sp[ARM_BANK_FIQ] = UINT32_C(0x0400FFFC);
        state.banked_lr[ARM_BANK_FIQ] = UINT32_C(0x04001000);

        const uint32_t return_link = UINT32_C(0x02002004);
        const uint32_t vector_base = UINT32_C(0xFFFF0000);
        if (!oracis_arm_cpu_enter_exception(
                &state, entry.exception, return_link, vector_base)) {
            return fail("valid exception entry was rejected");
        }

        uint32_t exception_bank = UINT32_MAX;
        if (!oracis_arm_cpu_mode_bank(entry.mode, &exception_bank)) {
            return fail("test exception mode is not mapped");
        }
        const uint32_t expected_vector = vector_base + entry.vector_offset;
        const uint32_t expected_mask =
            CPSR_I_BIT | (entry.masks_fiq ? CPSR_F_BIT : 0u);

        if ((state.cpsr & ORACIS_ARM_MODE_MASK) != entry.mode ||
            (state.cpsr & CPSR_T_BIT) != 0u ||
            (state.cpsr & expected_mask) != expected_mask ||
            (state.cpsr & CPSR_Z_BIT) == 0u ||
            state.banked_spsr[exception_bank] != original_cpsr ||
            state.R[14] != return_link ||
            state.R[15] != expected_vector) {
            return fail("exception entry did not save state/set masks/vector");
        }

        if (!oracis_arm_cpu_return_from_exception(&state, return_link)) {
            return fail("exception return failed");
        }
        if (state.cpsr != original_cpsr ||
            state.R[13] != UINT32_C(0x0207FFFC) ||
            state.R[14] != UINT32_C(0x02001000) ||
            state.R[15] != return_link ||
            state.banked_sp[exception_bank] == 0u) {
            return fail("exception return did not restore original CPU context");
        }
    }

    /* Normal and high vector tables share one base+offset calculation. */
    ArmCpuState low{};
    low.cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (!oracis_arm_cpu_enter_exception(
            &low, ORACIS_ARM_EXCEPTION_IRQ, 0x100u, 0u) ||
        low.R[15] != 0x18u) {
        return fail("normal IRQ vector address is incorrect");
    }

    /* Invalid inputs must be rejected without partially changing state. */
    ArmCpuState invalid{};
    invalid.cpsr = ORACIS_ARM_MODE_SYSTEM;
    invalid.R[15] = UINT32_C(0x02000000);
    const ArmCpuState before_invalid = invalid;
    if (oracis_arm_cpu_enter_exception(
            &invalid, static_cast<OracisArmException>(99), 0u, 0u) ||
        std::memcmp(&invalid, &before_invalid, sizeof(invalid)) != 0) {
        return fail("unknown exception type modified CPU state");
    }
    if (oracis_arm_cpu_enter_exception(
            &invalid, ORACIS_ARM_EXCEPTION_FIQ, 0u, UINT32_MAX) ||
        std::memcmp(&invalid, &before_invalid, sizeof(invalid)) != 0) {
        return fail("vector overflow modified CPU state");
    }
    if (oracis_arm_cpu_return_from_exception(&invalid, 0x44u)) {
        return fail("User/System mode must not return through an SPSR");
    }

    std::puts("ARM exception entry/return tests passed");
    return 0;
}
