#include "oracis/recomp/runtime_arm.h"

#include <cstdio>
#include <cstdint>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "runtime_abi_bridge_test: %s\n", message);
    return 1;
}

} // namespace

int main(void)
{
    using CoprocessorDataProcessing = void (*)(
        uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    CoprocessorDataProcessing volatile cdp_provider = &runtime_coproc_cdp;
    if (cdp_provider == nullptr) {
        return fail("CDP ABI provider is not linked");
    }

    ArmCpuState arm9 = {};
    ArmCpuState arm7 = {};

    arm9.cpsr = UINT32_C(0x1F);
    arm7.cpsr = UINT32_C(0x1F);

    if (!oracis_arm_runtime_bind_cpus(&arm9, &arm7))
        return fail("binding both CPU states failed");
    if (oracis_arm_runtime_current_state() != &arm9 ||
        g_nds_active != NDS_ARM9) {
        return fail("binding must select ARM9 initially");
    }

    g_cpu.R[0] = UINT32_C(0x11223344);
    arm_set_nz(UINT32_C(0x80000000));
    if (arm9.R[0] != UINT32_C(0x11223344) ||
        (arm9.cpsr & CPSR_N_BIT) == 0u ||
        (arm7.cpsr & CPSR_N_BIT) != 0u) {
        return fail("ARM9 g_cpu alias or flag wrapper selected the wrong state");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM7))
        return fail("selecting ARM7 failed");
    if (oracis_arm_runtime_current_state() != &arm7 ||
        g_nds_active != NDS_ARM7) {
        return fail("active CPU selector did not expose ARM7 state");
    }

    g_cpu.R[1] = UINT32_C(0x55667788);
    arm_set_nzcv_add(UINT32_C(0x7FFFFFFF), 1u, UINT32_C(0x80000000));
    if (arm7.R[1] != UINT32_C(0x55667788) ||
        (arm7.cpsr & (CPSR_N_BIT | CPSR_V_BIT)) !=
            (CPSR_N_BIT | CPSR_V_BIT) ||
        (arm9.cpsr & CPSR_V_BIT) != 0u) {
        return fail("ARM7 flag wrapper did not modify only ARM7");
    }

    /* Generated C's inline condition path and the out-of-line spelling agree. */
    g_cpu.cpsr = UINT32_C(0x12) | CPSR_Z_BIT;
    if (!arm_cond_passes_i(0x0u) || !arm_cond_passes(0x0u) ||
        arm_cond_passes_i(0x1u) || arm_cond_passes(0x1u)) {
        return fail("condition helpers disagree");
    }

    /* The global ABI wrappers route PSR operations through the selected CPU. */
    g_cpu.banked_spsr[ARM_BANK_IRQ] =
        ORACIS_ARM_MODE_SYSTEM | CPSR_T_BIT | CPSR_Z_BIT;
    runtime_msr_spsr(ORACIS_ARM_MODE_SYSTEM | CPSR_T_BIT, 0x8u);
    const uint32_t saved = runtime_mrs_spsr();
    if ((saved & (CPSR_T_BIT | CPSR_Z_BIT)) != CPSR_T_BIT ||
        runtime_mrs_cpsr() != g_cpu.cpsr) {
        return fail("PSR wrappers did not expose the selected CPU state");
    }

    /* Exception-return wrapper restores mode/flags and publishes R15. */
    g_cpu.cpsr = ORACIS_ARM_MODE_IRQ | CPSR_I_BIT;
    g_cpu.banked_spsr[ARM_BANK_IRQ] =
        ORACIS_ARM_MODE_SYSTEM | CPSR_T_BIT | CPSR_C_BIT;
    g_cpu.banked_sp[ARM_BANK_USER] = UINT32_C(0x0207FFFC);
    g_cpu.banked_lr[ARM_BANK_USER] = UINT32_C(0x02001000);
    runtime_exception_return(UINT32_C(0x02003002));
    if (g_cpu.cpsr != (ORACIS_ARM_MODE_SYSTEM | CPSR_T_BIT | CPSR_C_BIT) ||
        g_cpu.R[15] != UINT32_C(0x02003002) ||
        g_cpu.R[13] != UINT32_C(0x0207FFFC) ||
        g_cpu.R[14] != UINT32_C(0x02001000)) {
        return fail("exception-return wrapper did not restore ARM7 context");
    }

    /*
     * NDS BIOS SWI 09h performs signed r0/r1 division and returns
     * quotient, remainder and unsigned absolute quotient in r0/r1/r3.
     */
    const auto check_division = [](
        NdsCpu cpu, uint32_t numerator, uint32_t denominator,
        uint32_t quotient, uint32_t remainder, uint32_t magnitude) {
        if (!oracis_arm_runtime_select_cpu(cpu)) return false;
        ArmCpuState *state = oracis_arm_runtime_current_state();
        if (state == nullptr) return false;

        state->cpsr = ORACIS_ARM_MODE_SYSTEM | CPSR_Z_BIT | CPSR_C_BIT;
        const uint32_t saved_cpsr = state->cpsr;
        state->R[0] = numerator;
        state->R[1] = denominator;
        state->R[2] = UINT32_C(0xA5A5A5A5);
        state->R[3] = UINT32_C(0x5A5A5A5A);
        runtime_swi(0x09u);
        return state->R[0] == quotient &&
               state->R[1] == remainder &&
               state->R[2] == UINT32_C(0xA5A5A5A5) &&
               state->R[3] == magnitude &&
               state->cpsr == saved_cpsr;
    };

    if (!check_division(
            NDS_ARM9,
            static_cast<uint32_t>(-1234),
            10u,
            static_cast<uint32_t>(-123),
            static_cast<uint32_t>(-4),
            123u) ||
        !check_division(
            NDS_ARM7, 1234u,
            static_cast<uint32_t>(-10),
            static_cast<uint32_t>(-123), 4u, 123u) ||
        !check_division(
            NDS_ARM9, UINT32_C(0x80000000), UINT32_MAX,
            UINT32_C(0x80000000), 0u, UINT32_C(0x80000000)) ||
        !check_division(NDS_ARM7, 0u, 7u, 0u, 0u, 0u)) {
        return fail("NDS7/NDS9 BIOS signed division SWI semantics failed");
    }

    /*
     * NDS BIOS SWI 0Dh is unsigned floor(sqrt(r0)); verify perfect squares,
     * adjacent non-squares, high-bit inputs and the full uint32 range.
     */
    struct SqrtCase {
        uint32_t input;
        uint32_t expected;
    };
    const SqrtCase sqrt_cases[] = {
        {0u, 0u},
        {1u, 1u},
        {2u, 1u},
        {3u, 1u},
        {4u, 2u},
        {15u, 3u},
        {16u, 4u},
        {17u, 4u},
        {UINT32_C(0x80000000), 46340u},
        {UINT32_MAX, 65535u}
    };
    const NdsCpu sqrt_cpus[2] = {NDS_ARM9, NDS_ARM7};

    for (unsigned cpu_index = 0u; cpu_index < 2u; ++cpu_index) {
        const NdsCpu cpu = sqrt_cpus[cpu_index];
        if (!oracis_arm_runtime_select_cpu(cpu)) {
            return fail("CPU selection failed for BIOS square-root test");
        }
        ArmCpuState *state = oracis_arm_runtime_current_state();
        if (state == nullptr) {
            return fail("active CPU state was unavailable for BIOS square root");
        }
        for (const SqrtCase &test_case : sqrt_cases) {
            state->cpsr = ORACIS_ARM_MODE_SYSTEM | CPSR_N_BIT | CPSR_C_BIT;
            const uint32_t saved_cpsr = state->cpsr;
            state->R[0] = test_case.input;
            runtime_swi(0x0Du);
            const uint32_t result = state->R[0];
            if (result != test_case.expected ||
                static_cast<uint64_t>(result) * result > test_case.input ||
                static_cast<uint64_t>(result + 1u) * (result + 1u) <=
                    test_case.input ||
                state->cpsr != saved_cpsr) {
                return fail("NDS7/NDS9 BIOS unsigned square-root SWI failed");
            }
        }
    }

    if (oracis_arm_runtime_select_cpu(static_cast<NdsCpu>(77)))
        return fail("invalid CPU selector value must be rejected");

    std::puts("NDSRecomp-compatible Oracis runtime bridge tests passed");
    return 0;
}
