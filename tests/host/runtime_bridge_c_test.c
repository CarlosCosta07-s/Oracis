#include "oracis/host/runtime_bridge.h"
#include "oracis/memory/guest_memory_abi.h"
#include "oracis/recomp/static_dispatch_abi.h"
#include "oracis/recomp/runtime_arm.h"

#include <stdint.h>
#include <stdio.h>

static uint32_t g_arm9_bank_calls = 0u;
static uint32_t g_arm7_bank_calls = 0u;
static uint32_t g_arm9_irq_return_pc = 0u;
static uint32_t g_arm7_irq_return_pc = 0u;
static uint32_t g_arm9_irq_saved_cpsr = 0u;
static uint32_t g_arm7_irq_saved_cpsr = 0u;
static int g_arm9_irq_triggered = 0;
static int g_arm7_irq_triggered = 0;
static int g_arm9_irq_handler_ok = 0;
static int g_arm7_irq_handler_ok = 0;

enum {
    TEST_KEYPAD_IRQ = 1u << 12u,
    TEST_KEYPAD_A = 1u,
    TEST_KEYCNT_IRQ_ENABLE = 1u << 14u
};

static void arm9_irq_vector(void)
{
    ArmCpuState *state = oracis_active_cpu_state;
    g_arm9_irq_handler_ok =
        g_nds_active == NDS_ARM9 &&
        state != NULL &&
        (state->cpsr & UINT32_C(0x1F)) == UINT32_C(0x12) &&
        (state->cpsr & CPSR_T_BIT) == 0u &&
        (state->cpsr & CPSR_I_BIT) != 0u &&
        state->banked_spsr[ARM_BANK_IRQ] == g_arm9_irq_saved_cpsr &&
        state->R[14] == g_arm9_irq_return_pc + 4u &&
        state->R[15] == UINT32_C(0xFFFF0018);
    bus_write_u32(UINT32_C(0x04000214), TEST_KEYPAD_IRQ);
    runtime_exception_return(g_arm9_irq_return_pc);
}

static void arm7_irq_vector(void)
{
    ArmCpuState *state = oracis_active_cpu_state;
    g_arm7_irq_handler_ok =
        g_nds_active == NDS_ARM7 &&
        state != NULL &&
        (state->cpsr & UINT32_C(0x1F)) == UINT32_C(0x12) &&
        (state->cpsr & CPSR_T_BIT) == 0u &&
        (state->cpsr & CPSR_I_BIT) != 0u &&
        state->banked_spsr[ARM_BANK_IRQ] == g_arm7_irq_saved_cpsr &&
        state->R[14] == g_arm7_irq_return_pc + 4u &&
        state->R[15] == UINT32_C(0x00000018);
    bus_write_u32(UINT32_C(0x04000214), TEST_KEYPAD_IRQ);
    runtime_exception_return(g_arm7_irq_return_pc);
}

static void arm9_test_bank(void)
{
    ++g_arm9_bank_calls;
    if (!g_arm9_irq_triggered) {
        ArmCpuState *state = oracis_active_cpu_state;
        g_arm9_irq_triggered = 1;
        g_arm9_irq_return_pc = UINT32_C(0x02001004);
        g_arm9_irq_saved_cpsr = state->cpsr;
        state->R[15] = g_arm9_irq_return_pc;
        bus_write_u32(UINT32_C(0x04000210), TEST_KEYPAD_IRQ);
        bus_write_u32(UINT32_C(0x04000208), 1u);
        bus_write_u16(
            UINT32_C(0x04000132),
            (uint16_t)(TEST_KEYCNT_IRQ_ENABLE | TEST_KEYPAD_A));
        runtime_irq(g_arm9_irq_return_pc);
        if (state->cpsr != g_arm9_irq_saved_cpsr ||
            state->R[15] != g_arm9_irq_return_pc) {
            g_arm9_irq_handler_ok = 0;
        }
    }
    runtime_tick(1u);
}

static void arm9_resume_bank(void)
{
    ++g_arm9_bank_calls;
    runtime_tick(1u);
}

static void arm7_resume_bank(void)
{
    ++g_arm7_bank_calls;
    runtime_tick(1u);
}

static void arm7_test_bank(void)
{
    ++g_arm7_bank_calls;
    if (!g_arm7_irq_triggered) {
        ArmCpuState *state = oracis_active_cpu_state;
        g_arm7_irq_triggered = 1;
        g_arm7_irq_return_pc = UINT32_C(0x02381002);
        g_arm7_irq_saved_cpsr = state->cpsr;
        state->R[15] = g_arm7_irq_return_pc;
        bus_write_u32(UINT32_C(0x04000210), TEST_KEYPAD_IRQ);
        bus_write_u32(UINT32_C(0x04000208), 1u);
        bus_write_u16(
            UINT32_C(0x04000132),
            (uint16_t)(TEST_KEYCNT_IRQ_ENABLE | TEST_KEYPAD_A));
        runtime_tick(1u);
        if (state->cpsr != g_arm7_irq_saved_cpsr ||
            state->R[15] != g_arm7_irq_return_pc) {
            g_arm7_irq_handler_ok = 0;
        }
        return;
    }
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

    if (oracis_host_runtime_set_game_card_chip_id(UINT32_C(0x800000C2)) !=
        ORACIS_HOST_RUNTIME_GAME_CARD_ERROR) {
        fprintf(stderr, "host bridge accepted a chip ID before mounting an image\\n");
        return 1;
    }

    uint8_t synthetic_game_card[0x8200] = {0};
    synthetic_game_card[0x0C] = (uint8_t)'C';
    synthetic_game_card[0x0D] = (uint8_t)'P';
    synthetic_game_card[0x0E] = (uint8_t)'U';
    synthetic_game_card[0x0F] = (uint8_t)'E';
    synthetic_game_card[0x8000] = UINT8_C(0xA5);
    synthetic_game_card[0x8001] = UINT8_C(0x5A);
    if (oracis_host_runtime_mount_game_card_image(NULL, 0x200u) !=
            ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID ||
        oracis_host_runtime_mount_game_card_image(
            synthetic_game_card, 0x1FFu) != ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID ||
        oracis_host_runtime_mount_game_card_image(
            synthetic_game_card, UINT32_C(0x20000001)) != ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID ||
        oracis_host_runtime_mount_game_card_image(
            synthetic_game_card, (uint32_t)sizeof(synthetic_game_card)) !=
            ORACIS_HOST_RUNTIME_OK) {
        fprintf(stderr, "C host bridge rejected a valid synthetic Game Card image or accepted an invalid one\\n");
        return 1;
    }
    /* The bridge owns a copy: mutate the caller buffer before issuing a read. */
    synthetic_game_card[0x8000] = 0u;
    if (oracis_host_runtime_set_game_card_chip_id(UINT32_C(0x800000C2)) !=
        ORACIS_HOST_RUNTIME_OK) {
        fprintf(stderr, "host bridge could not configure explicit synthetic chip ID\\n");
        return 1;
    }

    uint8_t synthetic_arm7_bios[0x4000] = {0};
    for (uint32_t index = 0u; index < UINT32_C(0x1048); ++index) {
        synthetic_arm7_bios[0x30u + index] =
            (uint8_t)((index * 37u + 11u) & 0xFFu);
    }
    if (oracis_host_runtime_load_game_card_arm7_bios(
            synthetic_arm7_bios, UINT32_C(0x3FFF)) !=
            ORACIS_HOST_RUNTIME_INVALID_ARGUMENT ||
        oracis_host_runtime_load_game_card_arm7_bios(
            synthetic_arm7_bios, UINT32_C(0x4000)) != ORACIS_HOST_RUNTIME_OK) {
        fprintf(stderr, "host bridge failed to extract and initialize the synthetic ARM7 BIOS KEY1 table\n");
        return 1;
    }

    uint8_t secure_area_prefix[UINT32_C(0x800)] = {0};
    uint32_t secure_area_identifier_valid = UINT32_C(1);
    if (oracis_host_runtime_decrypt_game_card_secure_area_prefix(
            NULL, UINT32_C(0x800), &secure_area_identifier_valid) !=
            ORACIS_HOST_RUNTIME_INVALID_ARGUMENT ||
        oracis_host_runtime_decrypt_game_card_secure_area_prefix(
            secure_area_prefix, UINT32_C(0x800), NULL) !=
            ORACIS_HOST_RUNTIME_INVALID_ARGUMENT ||
        oracis_host_runtime_decrypt_game_card_secure_area_prefix(
            secure_area_prefix, UINT32_C(0x7FF),
            &secure_area_identifier_valid) != ORACIS_HOST_RUNTIME_INVALID_ARGUMENT) {
        fprintf(stderr, "C host bridge accepted invalid Secure Area prefix arguments\\n");
        return 1;
    }
    if (oracis_host_runtime_decrypt_game_card_secure_area_prefix(
            secure_area_prefix, UINT32_C(0x800),
            &secure_area_identifier_valid) != ORACIS_HOST_RUNTIME_OK ||
        secure_area_identifier_valid != 0u) {
        fprintf(stderr, "C host bridge failed to report an invalid synthetic Secure Area ID\\n");
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

    const uint8_t arm9_program[] = {
        0x00u, 0x00u, 0xA0u, 0xE1u,
        0x00u, 0x00u, 0xA0u, 0xE1u
    };
    const uint8_t arm7_program[] = {0x70u, 0x47u, 0xC0u, 0x46u};
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
    const NdsStaticValidation arm9_resume_validation = {
        UINT32_C(0x02001004), 4u, arm9_program + 4u, NULL, 0u
    };
    const NdsStaticValidation arm7_validation = {
        UINT32_C(0x02381000), (uint32_t)sizeof(arm7_program),
        arm7_program, NULL, 0u
    };
    const NdsDispatchEntry arm9_table[] = {
        {UINT32_C(0x02001000), 0u, &arm9_test_bank, &arm9_validation},
        {UINT32_C(0x02001004), 0u, &arm9_resume_bank, &arm9_resume_validation},
        {UINT32_C(0xFFFF0018), 0u, &arm9_irq_vector, NULL}
    };
    const NdsStaticValidation arm7_resume_validation = {
        UINT32_C(0x02381002), 2u, arm7_program + 2u, NULL, 0u
    };
    const NdsDispatchEntry arm7_table[] = {
        {UINT32_C(0x00000018), 0u, &arm7_irq_vector, NULL},
        {UINT32_C(0x02381000), 1u, &arm7_test_bank, &arm7_validation},
        {UINT32_C(0x02381002), 1u, &arm7_resume_bank, &arm7_resume_validation}
    };
    if (oracis_static_dispatch_register_bank(
            NDS_ARM9, arm9_table, 3u, "host-c-test-arm9") !=
            ORACIS_DISPATCH_REGISTER_OK ||
        oracis_static_dispatch_register_bank(
            NDS_ARM7, arm7_table, 3u, "host-c-test-arm7") !=
            ORACIS_DISPATCH_REGISTER_OK ||
        oracis_host_runtime_has_program_banks() != 1) {
        oracis_static_dispatch_clear();
        fprintf(stderr, "host bridge could not register both test banks\n");
        return 1;
    }

    /*
     * The public C ABI must reject duplicate table identity regardless of
     * the caller-provided bank name or length. The later preflight also checks
     * that neither rejected registration created a duplicate candidate.
     */
    if (oracis_static_dispatch_register_bank(
            NDS_ARM9, arm9_table, 3u, "host-c-test-arm9-alias") !=
            ORACIS_DISPATCH_REGISTER_DUPLICATE_BANK_TABLE ||
        oracis_static_dispatch_register_bank(
            NDS_ARM9, arm9_table, 2u, "host-c-test-arm9-short-alias") !=
            ORACIS_DISPATCH_REGISTER_DUPLICATE_BANK_TABLE) {
        oracis_static_dispatch_clear();
        fprintf(stderr, "C ABI accepted a duplicate static dispatch table\\n");
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

    const int irq_round_status =
        oracis_host_runtime_run_round(1u, NULL, NULL);
    const uint64_t irq_round_bus_cycles =
        oracis_host_runtime_global_bus_cycles();
    if (irq_round_status != ORACIS_HOST_RUNTIME_OK ||
        g_arm9_bank_calls != 2u ||
        g_arm7_bank_calls != 1u ||
        g_arm9_irq_handler_ok != 1 ||
        g_arm7_irq_handler_ok != 1 ||
        irq_round_bus_cycles != UINT64_C(1)) {
        oracis_static_dispatch_clear();
        fprintf(stderr,
            "C host bridge IRQ round mismatch: status=%d "
            "ARM9 calls=%u IRQ=%d triggered=%d; "
            "ARM7 calls=%u IRQ=%d triggered=%d; cycles=%llu\n",
            irq_round_status,
            g_arm9_bank_calls, g_arm9_irq_handler_ok, g_arm9_irq_triggered,
            g_arm7_bank_calls, g_arm7_irq_handler_ok, g_arm7_irq_triggered,
            (unsigned long long)irq_round_bus_cycles);
        return 1;
    }

    /* Run a real B7 transaction after the caller has changed its source buffer. */
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        oracis_static_dispatch_clear();
        fprintf(stderr, "host bridge could not select ARM9 for Game Card read\n");
        return 1;
    }
    bus_write_u16(UINT32_C(0x040001A0), UINT16_C(0xC000));
    const uint8_t game_card_command[8] = {
        UINT8_C(0xB7), 0u, 0u, UINT8_C(0x80), 0u, 0u, 0u, 0u
    };
    for (uint32_t index = 0u; index < 8u; ++index) {
        bus_write_u8(UINT32_C(0x040001A8) + index, game_card_command[index]);
    }
    bus_write_u32(UINT32_C(0x040001A4),
        (UINT32_C(7) << 24u) | (UINT32_C(1) << 29u) | (UINT32_C(1) << 31u));
    if ((bus_read_u32(UINT32_C(0x040001A4)) & (UINT32_C(1) << 31u)) == 0u ||
        oracis_host_runtime_run_round(40u, NULL, NULL) != ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_global_bus_cycles() != UINT64_C(41) ||
        !oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        bus_read_u32(UINT32_C(0x04100010)) != UINT32_C(0x00005AA5) ||
        (bus_read_u32(UINT32_C(0x040001A4)) &
            ((UINT32_C(1) << 31u) | (UINT32_C(1) << 23u))) != 0u ||
        (bus_read_u32(UINT32_C(0x04000214)) & (UINT32_C(1) << 19u)) == 0u) {
        oracis_static_dispatch_clear();
        fprintf(stderr, "host bridge did not preserve its copied Game Card image through a timed B7 read\n");
        return 1;
    }
    /* The 0x90 response uses the explicitly configured physical chip ID. */
    const uint8_t game_card_id_command[8] = {
        UINT8_C(0x90), 0u, 0u, 0u, 0u, 0u, 0u, 0u
    };
    for (uint32_t index = 0u; index < 8u; ++index) {
        bus_write_u8(UINT32_C(0x040001A8) + index, game_card_id_command[index]);
    }
    bus_write_u32(UINT32_C(0x040001A4),
        (UINT32_C(7) << 24u) | (UINT32_C(1) << 29u) | (UINT32_C(1) << 31u));
    if (oracis_host_runtime_run_round(40u, NULL, NULL) != ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_global_bus_cycles() != UINT64_C(81) ||
        !oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        bus_read_u32(UINT32_C(0x04100010)) != UINT32_C(0x800000C2) ||
        (bus_read_u32(UINT32_C(0x040001A4)) &
            ((UINT32_C(1) << 31u) | (UINT32_C(1) << 23u))) != 0u) {
        oracis_static_dispatch_clear();
        fprintf(stderr, "host bridge did not return the explicit Game Card chip ID through command 0x90\\n");
        return 1;
    }

    if (oracis_host_runtime_unmount_game_card_image() != ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_unmount_game_card_image() != ORACIS_HOST_RUNTIME_OK) {
        oracis_static_dispatch_clear();
        fprintf(stderr, "C host bridge failed to unmount Game Card image cleanly\n");
        return 1;
    }

    if (oracis_static_dispatch_unregister_bank(
            NDS_ARM9, arm9_table, 3u) == 0 ||
        oracis_static_dispatch_unregister_bank(
            NDS_ARM7, arm7_table, 3u) == 0 ||
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
    puts("C host runtime bridge lifecycle/keypad/2D scanline/ARM9-ARM7 IRQ tests passed");
    return 0;
}
