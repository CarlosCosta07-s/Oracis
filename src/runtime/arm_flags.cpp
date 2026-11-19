#include "oracis/recomp/arm_flags.h"

namespace {

constexpr uint32_t kNzcvMask =
    CPSR_N_BIT | CPSR_Z_BIT | CPSR_C_BIT | CPSR_V_BIT;

uint32_t nz_flags(uint32_t result)
{
    uint32_t flags = 0u;
    if ((result & UINT32_C(0x80000000)) != 0u) flags |= CPSR_N_BIT;
    if (result == 0u) flags |= CPSR_Z_BIT;
    return flags;
}

void write_nzcv(ArmCpuState *state, uint32_t result,
                bool carry, bool overflow)
{
    if (state == nullptr) return;
    uint32_t flags = nz_flags(result);
    if (carry) flags |= CPSR_C_BIT;
    if (overflow) flags |= CPSR_V_BIT;
    state->cpsr = (state->cpsr & ~kNzcvMask) | flags;
}

bool flag(uint32_t cpsr, uint32_t mask)
{
    return (cpsr & mask) != 0u;
}

} // namespace

extern "C" int oracis_arm_condition_passes(
    uint32_t cpsr,
    uint32_t condition)
{
    if (condition > 0xFu) return 0;

    const bool n = flag(cpsr, CPSR_N_BIT);
    const bool z = flag(cpsr, CPSR_Z_BIT);
    const bool c = flag(cpsr, CPSR_C_BIT);
    const bool v = flag(cpsr, CPSR_V_BIT);

    switch (condition) {
    case 0x0: return z;                  /* EQ */
    case 0x1: return !z;                 /* NE */
    case 0x2: return c;                  /* CS/HS */
    case 0x3: return !c;                 /* CC/LO */
    case 0x4: return n;                  /* MI */
    case 0x5: return !n;                 /* PL */
    case 0x6: return v;                  /* VS */
    case 0x7: return !v;                 /* VC */
    case 0x8: return c && !z;            /* HI */
    case 0x9: return !c || z;            /* LS */
    case 0xA: return n == v;             /* GE */
    case 0xB: return n != v;             /* LT */
    case 0xC: return !z && (n == v);     /* GT */
    case 0xD: return z || (n != v);      /* LE */
    case 0xE: return 1;                  /* AL */
    case 0xF: return 0;                  /* NV, reserved on ARMv4T/ARMv5TE */
    default: return 0;
    }
}

extern "C" void oracis_arm_set_nz(ArmCpuState *state, uint32_t result)
{
    if (state == nullptr) return;
    state->cpsr = (state->cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT)) |
                  nz_flags(result);
}

extern "C" void oracis_arm_set_nzc_logic(
    ArmCpuState *state,
    uint32_t result,
    uint32_t shifter_carry)
{
    if (state == nullptr) return;
    const uint32_t flags =
        nz_flags(result) | ((shifter_carry & 1u) ? CPSR_C_BIT : 0u);
    state->cpsr = (state->cpsr &
                   ~(CPSR_N_BIT | CPSR_Z_BIT | CPSR_C_BIT)) | flags;
}

extern "C" void oracis_arm_set_nzcv_add(
    ArmCpuState *state,
    uint32_t a,
    uint32_t b,
    uint32_t result)
{
    const uint64_t wide = static_cast<uint64_t>(a) +
                          static_cast<uint64_t>(b);
    const bool carry = (wide >> 32u) != 0u;
    const bool overflow =
        ((~(a ^ b) & (a ^ result)) & UINT32_C(0x80000000)) != 0u;
    write_nzcv(state, result, carry, overflow);
}

extern "C" void oracis_arm_set_nzcv_adc(
    ArmCpuState *state,
    uint32_t a,
    uint32_t b,
    uint32_t carry_in,
    uint32_t result)
{
    const uint64_t wide = static_cast<uint64_t>(a) +
                          static_cast<uint64_t>(b) +
                          static_cast<uint64_t>(carry_in & 1u);
    const bool carry = (wide >> 32u) != 0u;
    const bool overflow =
        ((~(a ^ b) & (a ^ result)) & UINT32_C(0x80000000)) != 0u;
    write_nzcv(state, result, carry, overflow);
}

extern "C" void oracis_arm_set_nzcv_sub(
    ArmCpuState *state,
    uint32_t a,
    uint32_t b,
    uint32_t result)
{
    const bool carry = a >= b;
    const bool overflow =
        (((a ^ b) & (a ^ result)) & UINT32_C(0x80000000)) != 0u;
    write_nzcv(state, result, carry, overflow);
}

extern "C" void oracis_arm_set_nzcv_sbc(
    ArmCpuState *state,
    uint32_t a,
    uint32_t b,
    uint32_t carry_in,
    uint32_t result)
{
    const uint32_t borrow = 1u - (carry_in & 1u);
    const uint64_t subtrahend = static_cast<uint64_t>(b) + borrow;
    const bool carry = static_cast<uint64_t>(a) >= subtrahend;
    const bool overflow =
        (((a ^ b) & (a ^ result)) & UINT32_C(0x80000000)) != 0u;
    write_nzcv(state, result, carry, overflow);
}
