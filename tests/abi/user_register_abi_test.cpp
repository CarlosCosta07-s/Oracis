#include "oracis/recomp/runtime_arm.h"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "user_register_abi_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    ArmCpuState arm9{};
    ArmCpuState arm7{};
    arm9.cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7.cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (!oracis_arm_runtime_bind_cpus(&arm9, &arm7))
        return fail("CPU bind failed");

    for (uint32_t reg = 8u; reg <= 12u; ++reg)
        arm9.R[reg] = UINT32_C(0x10000000) + reg;
    arm9.R[13] = UINT32_C(0x0207FFFC);
    arm9.R[14] = UINT32_C(0x02001000);

    /* User/System mode directly exposes the user bank. */
    if (runtime_read_user_reg(8u) != UINT32_C(0x10000008) ||
        runtime_read_user_reg(13u) != UINT32_C(0x0207FFFC) ||
        runtime_read_user_reg(14u) != UINT32_C(0x02001000)) {
        return fail("System mode did not read visible User registers");
    }

    /* A privileged non-FIQ mode shares R8-R12 but banks R13/R14. */
    if (!oracis_arm_cpu_switch_mode(&arm9, ORACIS_ARM_MODE_IRQ))
        return fail("System -> IRQ switch failed");
    arm9.R[13] = UINT32_C(0x03001000);
    arm9.R[14] = UINT32_C(0x03002000);
    runtime_write_user_reg(8u, UINT32_C(0x20000008));
    runtime_write_user_reg(12u, UINT32_C(0x2000000C));
    runtime_write_user_reg(13u, UINT32_C(0x0207E000));
    runtime_write_user_reg(14u, UINT32_C(0x0200A000));

    if (arm9.R[8] != UINT32_C(0x20000008) ||
        arm9.R[12] != UINT32_C(0x2000000C) ||
        arm9.R[13] != UINT32_C(0x03001000) ||
        arm9.R[14] != UINT32_C(0x03002000) ||
        runtime_read_user_reg(13u) != UINT32_C(0x0207E000) ||
        runtime_read_user_reg(14u) != UINT32_C(0x0200A000)) {
        return fail("IRQ mode User-bank access modified the wrong bank");
    }

    if (!oracis_arm_cpu_switch_mode(&arm9, ORACIS_ARM_MODE_SYSTEM))
        return fail("IRQ -> System switch failed");
    if (arm9.R[13] != UINT32_C(0x0207E000) ||
        arm9.R[14] != UINT32_C(0x0200A000) ||
        arm9.R[8] != UINT32_C(0x20000008) ||
        arm9.R[12] != UINT32_C(0x2000000C)) {
        return fail("System mode did not restore updated User-bank registers");
    }

    /* In FIQ, R8-R12 are banked; user accesses must not alter FIQ visibles. */
    if (!oracis_arm_cpu_switch_mode(&arm9, ORACIS_ARM_MODE_FIQ))
        return fail("System -> FIQ switch failed");
    for (uint32_t reg = 8u; reg <= 12u; ++reg)
        arm9.R[reg] = UINT32_C(0xF0000000) + reg;

    runtime_write_user_reg(8u, UINT32_C(0x30000008));
    runtime_write_user_reg(9u, UINT32_C(0x30000009));
    if (arm9.R[8] != UINT32_C(0xF0000008) ||
        arm9.R[9] != UINT32_C(0xF0000009) ||
        runtime_read_user_reg(8u) != UINT32_C(0x30000008) ||
        runtime_read_user_reg(9u) != UINT32_C(0x30000009)) {
        return fail("FIQ User R8-R12 access corrupted visible FIQ registers");
    }

    runtime_write_user_reg(13u, UINT32_C(0x0207D000));
    runtime_write_user_reg(14u, UINT32_C(0x0200B000));
    if (arm9.R[13] != 0u || arm9.R[14] != 0u ||
        runtime_read_user_reg(13u) != UINT32_C(0x0207D000) ||
        runtime_read_user_reg(14u) != UINT32_C(0x0200B000)) {
        return fail("FIQ User SP/LR access changed visible FIQ SP/LR");
    }

    if (!oracis_arm_cpu_switch_mode(&arm9, ORACIS_ARM_MODE_SYSTEM))
        return fail("FIQ -> System switch failed");
    if (arm9.R[8] != UINT32_C(0x30000008) ||
        arm9.R[9] != UINT32_C(0x30000009) ||
        arm9.R[13] != UINT32_C(0x0207D000) ||
        arm9.R[14] != UINT32_C(0x0200B000)) {
        return fail("updated User bank was not restored after FIQ");
    }

    /* ARM7 remains isolated from the ARM9 register state. */
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7))
        return fail("ARM7 selection failed");
    runtime_write_user_reg(8u, UINT32_C(0x70000008));
    if (runtime_read_user_reg(8u) != UINT32_C(0x70000008) ||
        arm9.R[8] == UINT32_C(0x70000008)) {
        return fail("active-CPU User-register wrapper crossed CPU states");
    }

    if (runtime_clz(0u) != 32u ||
        runtime_clz(1u) != 31u ||
        runtime_clz(UINT32_C(0x80000000)) != 0u ||
        runtime_clz(UINT32_C(0x00F00000)) != 8u) {
        return fail("CLZ helper returned incorrect leading-zero counts");
    }

    std::puts("User-bank register and CLZ ABI tests passed");
    return 0;
}
