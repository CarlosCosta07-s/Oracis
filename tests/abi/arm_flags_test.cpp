#include "oracis/recomp/arm_flags.h"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "arm_flags_test: %s\n", message);
    return 1;
}

bool expected_condition(uint32_t condition,
                        bool n, bool z, bool c, bool v)
{
    switch (condition) {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return c;
    case 0x3: return !c;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return c && !z;
    case 0x9: return !c || z;
    case 0xA: return n == v;
    case 0xB: return n != v;
    case 0xC: return !z && (n == v);
    case 0xD: return z || (n != v);
    case 0xE: return true;
    case 0xF: return false;
    default: return false;
    }
}

} // namespace

int main()
{
    /* Exhaust every N/Z/C/V combination against all 16 conditions. */
    for (uint32_t flags = 0; flags < 16u; ++flags) {
        const bool n = (flags & 0x8u) != 0u;
        const bool z = (flags & 0x4u) != 0u;
        const bool c = (flags & 0x2u) != 0u;
        const bool v = (flags & 0x1u) != 0u;
        uint32_t cpsr = ORACIS_ARM_MODE_SYSTEM |
                        (n ? CPSR_N_BIT : 0u) |
                        (z ? CPSR_Z_BIT : 0u) |
                        (c ? CPSR_C_BIT : 0u) |
                        (v ? CPSR_V_BIT : 0u);
        for (uint32_t condition = 0; condition < 16u; ++condition) {
            if ((oracis_arm_condition_passes(cpsr, condition) != 0) !=
                expected_condition(condition, n, z, c, v)) {
                return fail("condition evaluator disagreed with reference truth table");
            }
        }
    }
    if (oracis_arm_condition_passes(0u, 16u)) {
        return fail("condition values above 15 must be rejected");
    }

    ArmCpuState state{};
    state.cpsr = UINT32_C(0x12) | CPSR_T_BIT |
                 CPSR_I_BIT | CPSR_F_BIT | CPSR_V_BIT | CPSR_C_BIT;
    const uint32_t preserved = state.cpsr &
        ~(CPSR_N_BIT | CPSR_Z_BIT | CPSR_C_BIT | CPSR_V_BIT);

    oracis_arm_set_nz(&state, UINT32_C(0x80000000));
    if ((state.cpsr & CPSR_N_BIT) == 0u ||
        (state.cpsr & CPSR_Z_BIT) != 0u ||
        (state.cpsr & (CPSR_C_BIT | CPSR_V_BIT)) !=
            (CPSR_C_BIT | CPSR_V_BIT) ||
        (state.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT | CPSR_C_BIT | CPSR_V_BIT)) != preserved) {
        return fail("NZ update did not preserve unrelated CPSR bits");
    }

    oracis_arm_set_nzc_logic(&state, 0u, 0u);
    if ((state.cpsr & CPSR_Z_BIT) == 0u ||
        (state.cpsr & CPSR_N_BIT) != 0u ||
        (state.cpsr & CPSR_C_BIT) != 0u ||
        (state.cpsr & CPSR_V_BIT) == 0u) {
        return fail("logical NZC update was incorrect");
    }

    /* ADD: signed overflow, then unsigned carry + zero. */
    oracis_arm_set_nzcv_add(&state, UINT32_C(0x7FFFFFFF), 1u,
                            UINT32_C(0x80000000));
    if ((state.cpsr & (CPSR_N_BIT | CPSR_V_BIT)) !=
        (CPSR_N_BIT | CPSR_V_BIT) ||
        (state.cpsr & (CPSR_Z_BIT | CPSR_C_BIT)) != 0u) {
        return fail("ADD overflow flags were incorrect");
    }
    oracis_arm_set_nzcv_add(&state, UINT32_MAX, 1u, 0u);
    if ((state.cpsr & (CPSR_Z_BIT | CPSR_C_BIT)) !=
        (CPSR_Z_BIT | CPSR_C_BIT) ||
        (state.cpsr & (CPSR_N_BIT | CPSR_V_BIT)) != 0u) {
        return fail("ADD carry/zero flags were incorrect");
    }

    /* ADC: carry-in participates in the 33-bit result. */
    oracis_arm_set_nzcv_adc(&state, UINT32_MAX, 0u, 1u, 0u);
    if ((state.cpsr & (CPSR_Z_BIT | CPSR_C_BIT)) !=
        (CPSR_Z_BIT | CPSR_C_BIT)) {
        return fail("ADC carry-in flags were incorrect");
    }

    /* SUB: borrow is represented by C=0; signed overflow is independent. */
    oracis_arm_set_nzcv_sub(&state, 0u, 1u, UINT32_MAX);
    if ((state.cpsr & (CPSR_N_BIT | CPSR_C_BIT | CPSR_V_BIT)) != CPSR_N_BIT) {
        return fail("SUB borrow flags were incorrect");
    }
    oracis_arm_set_nzcv_sub(&state, UINT32_C(0x80000000), 1u,
                            UINT32_C(0x7FFFFFFF));
    if ((state.cpsr & (CPSR_C_BIT | CPSR_V_BIT)) !=
        (CPSR_C_BIT | CPSR_V_BIT)) {
        return fail("SUB overflow flags were incorrect");
    }

    /* SBC: C=1 means no incoming borrow; C=0 means subtract one extra. */
    oracis_arm_set_nzcv_sbc(&state, 5u, 3u, 0u, 1u);
    if ((state.cpsr & (CPSR_N_BIT | CPSR_Z_BIT | CPSR_C_BIT | CPSR_V_BIT)) != CPSR_C_BIT) {
        return fail("SBC no-borrow result flags were incorrect");
    }
    oracis_arm_set_nzcv_sbc(&state, 0u, 0u, 0u, UINT32_MAX);
    if ((state.cpsr & (CPSR_N_BIT | CPSR_C_BIT)) != CPSR_N_BIT) {
        return fail("SBC borrow result flags were incorrect");
    }

    std::puts("ARM condition and NZCV tests passed");
    return 0;
}
