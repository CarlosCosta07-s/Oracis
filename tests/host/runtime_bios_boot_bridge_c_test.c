#include "oracis/host/runtime_bridge.h"
#include "oracis/recomp/runtime_arm.h"
#include "oracis/recomp/static_dispatch_abi.h"

#include <stdint.h>
#include <stdio.h>

static uint32_t g_arm9_calls = 0u;
static uint32_t g_arm7_calls = 0u;

static void synthetic_arm9_reset_bank(void)
{
    ++g_arm9_calls;
    runtime_tick(16u);
}

static void synthetic_arm7_reset_bank(void)
{
    ++g_arm7_calls;
    runtime_tick(8u);
}

static int fail(const char *message)
{
    fprintf(stderr, "runtime_bios_boot_bridge_c_test: %s\n", message);
    return 1;
}

int main(void)
{
    uint8_t arm9_bios[0x1000] = {0};
    uint8_t arm7_bios[0x4000] = {0};
    for (uint32_t index = 0u; index < 0x1048u; ++index) {
        arm7_bios[0x30u + index] = (uint8_t)(index * 13u + 9u);
    }

    if (oracis_host_runtime_initialize() != ORACIS_HOST_RUNTIME_OK) {
        return fail("host runtime initialization failed");
    }
    if (oracis_host_runtime_load_arm9_bios(arm9_bios, sizeof(arm9_bios)) !=
            ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_load_game_card_arm7_bios(
            arm7_bios, sizeof(arm7_bios)) != ORACIS_HOST_RUNTIME_OK) {
        return fail("synthetic BIOS images could not be loaded through the C ABI");
    }

    OracisHostRuntimeEntryPointReport arm9_report = {0};
    OracisHostRuntimeEntryPointReport arm7_report = {0};
    if (oracis_host_runtime_start_retail_bios_boot(
            &arm9_report, &arm7_report) != ORACIS_HOST_RUNTIME_NO_PROGRAM_BANKS ||
        oracis_host_runtime_run_retail_bios_round(8u, NULL, NULL) !=
            ORACIS_HOST_RUNTIME_BIOS_BOOT_NOT_ACTIVE ||
        oracis_host_runtime_global_bus_cycles() != 0u) {
        return fail("boot path did not fail closed before any BIOS banks were registered");
    }

    const NdsDispatchEntry arm9_bank[] = {
        {UINT32_C(0xFFFF0000), 0u, &synthetic_arm9_reset_bank, NULL}
    };
    const NdsDispatchEntry arm7_bank[] = {
        {UINT32_C(0x00000000), 0u, &synthetic_arm7_reset_bank, NULL}
    };
    if (oracis_static_dispatch_register_bank(
            NDS_ARM9, arm9_bank, 1u, "synthetic-user-arm9-bios") !=
            ORACIS_DISPATCH_REGISTER_OK ||
        oracis_static_dispatch_register_bank(
            NDS_ARM7, arm7_bank, 1u, "synthetic-user-arm7-bios") !=
            ORACIS_DISPATCH_REGISTER_OK) {
        return fail("synthetic native BIOS dispatch tables could not be registered");
    }

    if (oracis_host_runtime_start_retail_bios_boot(
            &arm9_report, &arm7_report) != ORACIS_HOST_RUNTIME_OK ||
        arm9_report.resolved != 1u ||
        arm9_report.pc != UINT32_C(0xFFFF0000) ||
        arm7_report.resolved != 1u ||
        arm7_report.pc != UINT32_C(0x00000000)) {
        return fail("BIOS reset-vector preflight did not resolve both native banks");
    }

    if (oracis_host_runtime_run_retail_bios_round(8u, NULL, NULL) !=
            ORACIS_HOST_RUNTIME_OK ||
        g_arm9_calls != 1u || g_arm7_calls != 1u ||
        oracis_host_runtime_global_bus_cycles() != UINT64_C(8)) {
        return fail("both BIOS CPU reset banks did not execute in the first scheduler round");
    }

    if (oracis_host_runtime_release_arm7_from_reset() !=
            ORACIS_HOST_RUNTIME_CPU_NOT_IN_RESET ||
        oracis_host_runtime_release_arm7_from_reset() !=
            ORACIS_HOST_RUNTIME_CPU_NOT_IN_RESET) {
        return fail("legacy ARM7 release hook changed the parallel reset state");
    }
    if (oracis_host_runtime_run_retail_bios_round(8u, NULL, NULL) !=
            ORACIS_HOST_RUNTIME_OK ||
        g_arm9_calls != 2u || g_arm7_calls != 2u ||
        oracis_host_runtime_global_bus_cycles() != UINT64_C(16)) {
        return fail("both BIOS CPUs did not continue synchronized execution");
    }

    if (oracis_host_runtime_shutdown() != ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_has_program_banks() != 0) {
        return fail("runtime shutdown did not remove temporary BIOS banks");
    }

    puts("C host BIOS boot bridge preflight/reset/scheduler tests passed");
    return 0;
}
