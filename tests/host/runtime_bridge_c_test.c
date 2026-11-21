#include "oracis/host/runtime_bridge.h"
#include "oracis/memory/guest_memory_abi.h"
#include "oracis/recomp/static_dispatch_abi.h"

#include <stdint.h>
#include <stdio.h>

static uint32_t g_arm9_bank_calls = 0u;
static uint32_t g_arm7_bank_calls = 0u;

static void arm9_test_bank(void)
{
    ++g_arm9_bank_calls;
    runtime_tick(1u);
}

static void arm7_test_bank(void)
{
    ++g_arm7_bank_calls;
    runtime_tick(1u);
}

int main(void)
{
    /* Host input can arrive before NdsMachine binds its keypad device. */
    oracis_host_set_keypad_state(UINT16_C(1));

    if (oracis_host_runtime_is_initialized() != 0) {
        fprintf(stderr, "host runtime was initialized before the test\n");
        return 1;
    }

    if (oracis_host_runtime_initialize() != ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_is_initialized() != 1 ||
        oracis_host_runtime_global_bus_cycles() != UINT64_C(0)) {
        fprintf(stderr, "C host bridge failed to initialize NdsMachine\n");
        return 1;
    }

    uint8_t synthetic_game_card[0x200] = {0};
    synthetic_game_card[0] = UINT8_C(0xA5);
    synthetic_game_card[1] = UINT8_C(0x5A);
    if (oracis_host_runtime_mount_game_card_image(NULL, 0x200u) !=
            ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID ||
        oracis_host_runtime_mount_game_card_image(
            synthetic_game_card, 0x1FFu) != ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID ||
        oracis_host_runtime_mount_game_card_image(
            synthetic_game_card, (uint32_t)sizeof(synthetic_game_card)) !=
            ORACIS_HOST_RUNTIME_OK) {
        fprintf(stderr, "C host bridge rejected a valid synthetic Game Card image or accepted an invalid one\\n");
        return 1;
    }
    /* The bridge owns a copy: callers can immediately reuse their input buffer. */
    synthetic_game_card[0] = 0u;
    if (oracis_host_runtime_unmount_game_card_image() != ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_unmount_game_card_image() != ORACIS_HOST_RUNTIME_OK) {
        fprintf(stderr, "C host bridge failed to unmount Game Card image cleanly\\n");
        return 1;
    }

    if (bus_read_u16(UINT32_C(0x04000130)) != UINT16_C(0x03FE)) {
        fprintf(stderr, "pre-bind host key state did not reach KEYINPUT\n");
        return 1;
    }

    uint16_t main_scanline[256] = {0};
    uint16_t sub_scanline[256] = {0};
    OracisHostVideoScanlineReport video_report = {0};
    if (oracis_host_runtime_render_scanline(
            0u, 0u, main_scanline, 256u, &video_report) !=
            ORACIS_HOST_RUNTIME_OK ||
        video_report.status != 0u ||
        main_scanline[0] != UINT16_C(0)) {
        fprintf(stderr, "C host bridge failed to compose Main Engine A backdrop\n");
        return 1;
    }
    if (oracis_host_runtime_render_scanline(
            1u, 191u, sub_scanline, 256u, &video_report) !=
            ORACIS_HOST_RUNTIME_OK ||
        video_report.status != 0u ||
        sub_scanline[255] != UINT16_C(0)) {
        fprintf(stderr, "C host bridge failed to compose Sub Engine B scanline\n");
        return 1;
    }

    const uint16_t preserved_scanline[256] = {UINT16_C(0x4321)};
    uint16_t error_scanline[256];
    for (unsigned x = 0u; x < 256u; ++x) {
        error_scanline[x] = preserved_scanline[x];
    }
    if (oracis_host_runtime_render_scanline(
            0u, 192u, error_scanline, 256u, &video_report) !=
            ORACIS_HOST_RUNTIME_INVALID_ARGUMENT ||
        error_scanline[0] != UINT16_C(0x4321)) {
        fprintf(stderr, "C host bridge did not validate scanline bounds transactionally\n");
        return 1;
    }
    if (oracis_host_runtime_render_scanline(
            0u, 0u, error_scanline, 255u, &video_report) !=
            ORACIS_HOST_RUNTIME_INVALID_ARGUMENT ||
        error_scanline[0] != UINT16_C(0x4321)) {
        fprintf(stderr, "C host bridge did not validate scanline buffer capacity\n");
        return 1;
    }

    if (oracis_host_runtime_initialize() !=
        ORACIS_HOST_RUNTIME_ALREADY_INITIALIZED) {
        fprintf(stderr, "C host bridge accepted a duplicate initialization\n");
        return 1;
    }

    if (oracis_host_runtime_register_compiled_banks() !=
        ORACIS_HOST_RUNTIME_NO_COMPILED_BANKS) {
        fprintf(stderr, "test build unexpectedly exposed compiled Platinum banks\n");
        return 1;
    }

    const uint8_t arm9_program[] = {0x00u, 0x00u, 0xA0u, 0xE1u};
    const uint8_t arm7_program[] = {0x70u, 0x47u};
    const OracisHostProgramSegment arm9_image = {
        UINT32_C(0x02001000),
        (uint32_t)sizeof(arm9_program),
        UINT32_C(0x02001000),
        0u,
        arm9_program
    };
    const OracisHostProgramSegment arm7_invalid_image = {
        UINT32_C(0x05000000),
        (uint32_t)sizeof(arm7_program),
        UINT32_C(0x05000000),
        1u,
        arm7_program
    };
    if (oracis_host_runtime_load_program_segments(
            &arm9_image, &arm7_invalid_image) !=
            ORACIS_HOST_RUNTIME_MACHINE_ERROR ||
        oracis_host_runtime_program_loaded() != 0 ||
        bus_read_u32(UINT32_C(0x02001000)) != UINT32_C(0)) {
        fprintf(stderr, "C host bridge partially loaded an invalid ARM9/ARM7 pair\n");
        return 1;
    }

    if (oracis_host_runtime_has_program_banks() != 0 ||
        oracis_host_runtime_run_round(1u, NULL, NULL) !=
            ORACIS_HOST_RUNTIME_NO_PROGRAM_BANKS) {
        fprintf(stderr, "host bridge ran without both static CPU banks\n");
        return 1;
    }

    const NdsStaticValidation arm9_validation = {
        UINT32_C(0x02001000), (uint32_t)sizeof(arm9_program),
        arm9_program, NULL, 0u
    };
    const NdsStaticValidation arm7_validation = {
        UINT32_C(0x02381000), (uint32_t)sizeof(arm7_program),
        arm7_program, NULL, 0u
    };
    const NdsDispatchEntry arm9_table[] = {
        {UINT32_C(0x02001000), 0u, &arm9_test_bank, &arm9_validation}
    };
    const NdsDispatchEntry arm7_table[] = {
        {UINT32_C(0x02381000), 1u, &arm7_test_bank, &arm7_validation}
    };
    if (oracis_static_dispatch_register_bank(
            NDS_ARM9, arm9_table, 1u, "host-c-test-arm9") !=
            ORACIS_DISPATCH_REGISTER_OK ||
        oracis_static_dispatch_register_bank(
            NDS_ARM7, arm7_table, 1u, "host-c-test-arm7") !=
            ORACIS_DISPATCH_REGISTER_OK ||
        oracis_host_runtime_has_program_banks() != 1) {
        oracis_static_dispatch_clear();
        fprintf(stderr, "host bridge could not register both test banks\n");
        return 1;
    }

    if (oracis_host_runtime_program_loaded() != 0 ||
        oracis_host_runtime_run_round(1u, NULL, NULL) !=
            ORACIS_HOST_RUNTIME_NO_PROGRAM_LOADED ||
        oracis_host_runtime_global_bus_cycles() != UINT64_C(0)) {
        oracis_static_dispatch_clear();
        fprintf(stderr, "host bridge executed before program images were loaded\n");
        return 1;
    }

    const OracisHostProgramSegment arm7_image = {
        UINT32_C(0x02381000),
        (uint32_t)sizeof(arm7_program),
        UINT32_C(0x02381000),
        1u,
        arm7_program
    };
    if (oracis_host_runtime_load_program_segments(
            &arm9_image, &arm7_image) != ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_program_loaded() != 1 ||
        bus_read_u32(UINT32_C(0x02001000)) != UINT32_C(0xE1A00000)) {
        fprintf(stderr, "C host bridge failed to load and verify ARM9 image\n");
        return 1;
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7) ||
        bus_read_u16(UINT32_C(0x02381000)) != UINT16_C(0x4770) ||
        !oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        fprintf(stderr, "C host bridge failed to load/verify ARM7 private image\n");
        return 1;
    }
    OracisHostRuntimeEntryPointReport arm9_entry = {0};
    OracisHostRuntimeEntryPointReport arm7_entry = {0};
    if (oracis_host_runtime_validate_entrypoints(
            &arm9_entry, &arm7_entry) != ORACIS_HOST_RUNTIME_OK ||
        arm9_entry.pc != UINT32_C(0x02001000) ||
        arm9_entry.resolved != 1u ||
        arm9_entry.lookup_status != ORACIS_DISPATCH_LOOKUP_FOUND ||
        arm9_entry.candidate_count != 1u ||
        arm7_entry.pc != UINT32_C(0x02381000) ||
        arm7_entry.resolved != 1u ||
        arm7_entry.lookup_status != ORACIS_DISPATCH_LOOKUP_FOUND ||
        arm7_entry.candidate_count != 1u ||
        (arm7_entry.cpsr & CPSR_T_BIT) == 0u) {
        fprintf(stderr, "host bridge failed to preflight both ARM/Thumb entry points\n");
        return 1;
    }

    if (oracis_host_runtime_validate_entrypoints(NULL, &arm7_entry) !=
        ORACIS_HOST_RUNTIME_INVALID_ARGUMENT) {
        fprintf(stderr, "host bridge accepted a null entry-point report\\n");
        return 1;
    }

    /* A registered row is inactive when its expected bytes no longer match. */
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7)) {
        fprintf(stderr, "host bridge could not select ARM7 for identity test\\n");
        return 1;
    }
    bus_write_u16(UINT32_C(0x02381000), UINT16_C(0x4771));
    if (oracis_host_runtime_validate_entrypoints(
            &arm9_entry, &arm7_entry) != ORACIS_HOST_RUNTIME_OK ||
        arm9_entry.resolved != 1u ||
        arm7_entry.resolved != 0u ||
        arm7_entry.lookup_status != ORACIS_DISPATCH_LOOKUP_MISS ||
        arm7_entry.candidate_count != 1u ||
        arm7_entry.has_inactive_candidate != 1u) {
        bus_write_u16(UINT32_C(0x02381000), UINT16_C(0x4770));
        (void)oracis_arm_runtime_select_cpu(NDS_ARM9);
        fprintf(stderr, "host bridge did not reject stale ARM7 entry bytes\\n");
        return 1;
    }
    bus_write_u16(UINT32_C(0x02381000), UINT16_C(0x4770));
    if (oracis_host_runtime_validate_entrypoints(
            &arm9_entry, &arm7_entry) != ORACIS_HOST_RUNTIME_OK ||
        arm9_entry.resolved != 1u || arm7_entry.resolved != 1u ||
        arm7_entry.has_inactive_candidate != 0u) {
        (void)oracis_arm_runtime_select_cpu(NDS_ARM9);
        fprintf(stderr, "host bridge did not restore ARM7 entry identity\\n");
        return 1;
    }

    /* The lookup mode comes from CPSR.T, not from an address low bit. */
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7)) {
        fprintf(stderr, "host bridge could not select ARM7 for mode preflight\\n");
        return 1;
    }
    oracis_active_cpu_state->cpsr &= ~CPSR_T_BIT;
    if (oracis_host_runtime_validate_entrypoints(
            &arm9_entry, &arm7_entry) != ORACIS_HOST_RUNTIME_OK ||
        arm7_entry.resolved != 0u ||
        arm7_entry.lookup_status != ORACIS_DISPATCH_LOOKUP_MISS ||
        arm7_entry.candidate_count != 0u) {
        oracis_active_cpu_state->cpsr |= CPSR_T_BIT;
        (void)oracis_arm_runtime_select_cpu(NDS_ARM9);
        fprintf(stderr, "host bridge ignored ARM7 CPSR.T during entry preflight\n");
        return 1;
    }
    oracis_active_cpu_state->cpsr |= CPSR_T_BIT;
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        fprintf(stderr, "host bridge could not restore ARM9 selection\n");
        return 1;
    }

    if (oracis_host_runtime_load_program_segments(
            &arm9_image, &arm7_image) != ORACIS_HOST_RUNTIME_MACHINE_ERROR) {
        fprintf(stderr, "C host bridge accepted a duplicate program load\n");
        return 1;
    }

    if (oracis_host_runtime_run_round(1u, NULL, NULL) !=
            ORACIS_HOST_RUNTIME_OK ||
        g_arm9_bank_calls != 2u ||
        g_arm7_bank_calls != 1u ||
        oracis_host_runtime_global_bus_cycles() != UINT64_C(1)) {
        oracis_static_dispatch_clear();
        fprintf(stderr, "C host bridge failed to execute synchronized ARM9/ARM7 banks\n");
        return 1;
    }

    if (oracis_static_dispatch_unregister_bank(
            NDS_ARM9, arm9_table, 1u) == 0 ||
        oracis_static_dispatch_unregister_bank(
            NDS_ARM7, arm7_table, 1u) == 0 ||
        oracis_host_runtime_has_program_banks() != 0) {
        oracis_static_dispatch_clear();
        fprintf(stderr, "host C test banks were not unregistered\n");
        return 1;
    }

    if (oracis_host_runtime_shutdown() != ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_is_initialized() != 0 ||
        oracis_host_runtime_last_machine_status() != 0) {
        fprintf(stderr, "C host bridge failed to shut down NdsMachine\n");
        return 1;
    }

    if (oracis_host_runtime_shutdown() != ORACIS_HOST_RUNTIME_OK) {
        fprintf(stderr, "C host bridge shutdown was not idempotent\n");
        return 1;
    }

    oracis_host_set_keypad_state(0u);
    puts("C host runtime bridge lifecycle/keypad/2D scanline tests passed");
    return 0;
}
