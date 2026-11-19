#include "runtime_arm.h"

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

    if (oracis_arm_runtime_select_cpu(static_cast<NdsCpu>(77)))
        return fail("invalid CPU selector value must be rejected");

    std::puts("NDSRecomp-compatible Oracis runtime bridge tests passed");
    return 0;
}
