#include "oracis/recomp/runtime_arm.h"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "runtime_arm7_cycle_combine_test: %s\n", message);
    return 1;
}

bool check(const char *label, uint32_t expected, uint32_t actual)
{
    if (expected == actual) return true;
    std::fprintf(stderr,
        "runtime_arm7_cycle_combine_test: %s expected=%u actual=%u\n",
        label, expected, actual);
    return false;
}

bool begin_instruction(ArmCpuState &arm7, uint32_t pc, bool thumb)
{
    arm7.cpsr = ORACIS_ARM_MODE_SYSTEM | (thumb ? CPSR_T_BIT : 0u);
    return runtime_code_cycles(pc) == 0u;
}

} // namespace

int main()
{
    ArmCpuState arm9{};
    ArmCpuState arm7{};
    arm9.cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7.cpsr = ORACIS_ARM_MODE_SYSTEM;

    if (!oracis_arm_runtime_bind_cpus(&arm9, &arm7)) {
        return fail("CPU binding failed");
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7)) {
        return fail("ARM7 selection failed");
    }

    if (!check("zero-cost resync", 0u,
               arm7_cycle_combine(0u, 0u, 0u, 0u))) {
        return 1;
    }

    /* No-data C class replaces the generated 1S with the region's true S. */
    if (!begin_instruction(arm7, UINT32_C(0x02000000), false) ||
        !check("main RAM ARM C", 2u,
               arm7_cycle_combine(1u, 0u, 0u, 0u))) {
        return fail("main RAM ARM C timing was incorrect");
    }
    if (!begin_instruction(arm7, UINT32_C(0x03000000), false) ||
        !check("WRAM ARM C", 1u,
               arm7_cycle_combine(1u, 0u, 0u, 0u))) {
        return fail("WRAM ARM C timing was incorrect");
    }
    if (!begin_instruction(arm7, UINT32_C(0x02000000), true) ||
        !check("main RAM Thumb C", 1u,
               arm7_cycle_combine(1u, 0u, 0u, 0u))) {
        return fail("main RAM Thumb C timing was incorrect");
    }

    /* CI classes replace the baked sequential fetch with a non-sequential one. */
    if (!begin_instruction(arm7, UINT32_C(0x02000000), false) ||
        !check("main RAM ARM CI", 11u,
               arm7_cycle_combine(3u, 0u, 0u, 1u))) {
        return fail("main RAM ARM CI timing was incorrect");
    }
    if (!begin_instruction(arm7, UINT32_C(0x02000000), true) ||
        !check("main RAM Thumb CI", 10u,
               arm7_cycle_combine(3u, 0u, 0u, 1u))) {
        return fail("main RAM Thumb CI timing was incorrect");
    }
    if (!begin_instruction(arm7, UINT32_C(0x03000000), false) ||
        !check("WRAM ARM CI", 3u,
               arm7_cycle_combine(3u, 0u, 0u, 1u))) {
        return fail("WRAM ARM CI timing was incorrect");
    }

    /* Main-RAM CODE and DATA use different NDS7/CODE and NDS7/DATA values. */
    if (!begin_instruction(arm7, UINT32_C(0x02000000), false)) {
        return fail("main/main instruction context failed");
    }
    const uint32_t main_data =
        runtime_mem_cycles(UINT32_C(0x02000000), 4u, 0u);
    if (!check("main/main load", 19u,
               arm7_cycle_combine(1u + main_data, main_data, 1u, 0u)) ||
        !check("main/main store", 19u,
               arm7_cycle_combine(1u + thumb_main_data, thumb_main_data, 0u, 0u))) {
        return fail("main/main data overlap timing was incorrect");
    }

    if (!begin_instruction(arm7, UINT32_C(0x02000000), false)) {
        return fail("main/WRAM instruction context failed");
    }
    const uint32_t fast_data =
        runtime_mem_cycles(UINT32_C(0x03000000), 4u, 0u);
    if (!check("main/WRAM store", 9u,
               arm7_cycle_combine(1u + fast_data, fast_data, 0u, 0u)) ||
        !check("main/WRAM load", 9u,
               arm7_cycle_combine(1u + fast_data, fast_data, 1u, 0u))) {
        return fail("main/WRAM overlap timing was incorrect");
    }

    if (!begin_instruction(arm7, UINT32_C(0x03000000), false)) {
        return fail("WRAM/main instruction context failed");
    }
    const uint32_t main_data_after_wram_code =
        runtime_mem_cycles(UINT32_C(0x02000000), 4u, 0u);
    if (!check("WRAM/main store", 10u,
               arm7_cycle_combine(1u + main_data, main_data, 0u, 0u)) ||
        !check("WRAM/main load", 11u,
               arm7_cycle_combine(1u + main_data_after_wram_code,
                                  main_data_after_wram_code, 1u, 0u))) {
        return fail("WRAM/main overlap timing was incorrect");
    }

    /* Non-main CODE/DATA keeps a distinct extra cycle for loads. */
    if (!begin_instruction(arm7, UINT32_C(0x03000000), false)) {
        return fail("WRAM/WRAM instruction context failed");
    }
    const uint32_t fast_data_after_wram_code =
        runtime_mem_cycles(UINT32_C(0x03000000), 4u, 0u);
    if (!check("WRAM/WRAM store", 2u,
               arm7_cycle_combine(1u + fast_data_after_wram_code,
                                  fast_data_after_wram_code, 0u, 0u)) ||
        !check("WRAM/WRAM load", 3u,
               arm7_cycle_combine(1u + fast_data_after_wram_code,
                                  fast_data_after_wram_code, 1u, 0u))) {
        return fail("WRAM/WRAM timing was incorrect");
    }

    /* VRAM C/D is a 16-bit code bus: ARM fetches split into two halfwords. */
    if (!begin_instruction(arm7, UINT32_C(0x06000000), false)) {
        return fail("VRAM/WRAM instruction context failed");
    }
    const uint32_t fast_data_after_vram_code =
        runtime_mem_cycles(UINT32_C(0x03000000), 4u, 0u);
    if (!check("VRAM/WRAM store", 3u,
               arm7_cycle_combine(1u + fast_data_after_vram_code,
                                  fast_data_after_vram_code, 0u, 0u)) ||
        !check("VRAM/WRAM load", 4u,
               arm7_cycle_combine(1u + fast_data_after_vram_code,
                                  fast_data_after_vram_code, 1u, 0u))) {
        return fail("VRAM/WRAM timing was incorrect");
    }

    /* Thumb main-RAM CODE uses N16=8, not the ARM N32=9. */
    if (!begin_instruction(arm7, UINT32_C(0x02000000), true)) {
        return fail("main Thumb instruction context failed");
    }
    const uint32_t thumb_main_data =
        runtime_mem_cycles(UINT32_C(0x02000000), 4u, 0u);
    if (!check("main Thumb/main data", 18u,
               arm7_cycle_combine(1u + main_data, main_data, 0u, 0u))) {
        return fail("main Thumb/main data timing was incorrect");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 reselection failed");
    }
    if (!oracis_arm_runtime_unbind_cpus(&arm9, &arm7)) {
        return fail("CPU unbinding failed");
    }

    std::puts("ARM7 instruction/data cycle-combine tests passed");
    return 0;
}
