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
static int g_request_active_destructor_registration = 0;
static int g_active_destructor_registration_status =
    ORACIS_HOST_RUNTIME_INVALID_ARGUMENT;

static void lifecycle_dispatch_entry(void)
{
}

/* Register stack-owned C tables, shut down while they are alive, then return. */
static int shutdown_with_temporary_dispatch_tables(void)
{
    const NdsDispatchEntry arm9_table[] = {
        {UINT32_C(0x02004000), 0u, &lifecycle_dispatch_entry, NULL}
    };
    const NdsDispatchEntry arm7_table[] = {
        {UINT32_C(0x02384000), 0u, &lifecycle_dispatch_entry, NULL}
    };

    if (oracis_static_dispatch_register_bank(
            NDS_ARM9, arm9_table, 1u, "lifecycle-stack-arm9") !=
            ORACIS_DISPATCH_REGISTER_OK ||
        oracis_static_dispatch_register_bank(
            NDS_ARM7, arm7_table, 1u, "lifecycle-stack-arm7") !=
            ORACIS_DISPATCH_REGISTER_OK ||
        oracis_host_runtime_has_program_banks() != 1) {
        (void)oracis_host_runtime_shutdown();
        fprintf(stderr, "lifecycle test could not register temporary C tables\n");
        return 1;
    }

    if (oracis_host_runtime_shutdown() != ORACIS_HOST_RUNTIME_OK) {
        fprintf(stderr, "lifecycle test could not shut down with temporary C tables\n");
        return 1;
    }
    return 0;
}

enum {
    TEST_KEYPAD_IRQ = 1u << 12u,
    TEST_KEYPAD_A = 1u,
    TEST_KEYCNT_IRQ_ENABLE = 1u << 14u,
    TEST_DTOR_HEAD = 0x02005000u,
    TEST_DTOR_NODE = 0x02005020u,
    TEST_DTOR_OBJECT = 0x02005040u,
    TEST_DTOR_FUNCTION = 0x02001004u
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

    if (g_request_active_destructor_registration) {
        ArmCpuState *state = oracis_active_cpu_state;
        const uint32_t old_r0 = state->R[0];
        const uint32_t old_r1 = state->R[1];
        const uint32_t old_r2 = state->R[2];
        state->R[0] = TEST_DTOR_OBJECT;
        state->R[1] = TEST_DTOR_FUNCTION;
        state->R[2] = TEST_DTOR_NODE;
        g_active_destructor_registration_status =
            oracis_host_runtime_register_active_global_destructor(
                TEST_DTOR_HEAD);
        state->R[0] = old_r0;
        state->R[1] = old_r1;
        state->R[2] = old_r2;
        g_request_active_destructor_registration = 0;
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

    if (oracis_host_runtime_register_global_destructor(
            (uint32_t)NDS_ARM9, UINT32_C(0x02001000),
            UINT32_C(0x02001020), UINT32_C(0x02001031),
            UINT32_C(0x02001040)) != ORACIS_HOST_RUNTIME_NO_PROGRAM_LOADED ||
        oracis_host_runtime_register_active_global_destructor(
            UINT32_C(0x02001000)) != ORACIS_HOST_RUNTIME_NO_PROGRAM_LOADED) {
        fprintf(stderr, "C destructor bridge accepted registration before program load\\n");
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

    uint8_t synthetic_firmware[0x100];
    for (uint32_t index = 0u; index < (uint32_t)sizeof(synthetic_firmware); ++index) {
        synthetic_firmware[index] = (uint8_t)(UINT32_C(0xA0) + index);
    }
    if (oracis_host_runtime_set_spi_firmware_image(
            NULL, (uint32_t)sizeof(synthetic_firmware)) !=
            ORACIS_HOST_RUNTIME_INVALID_ARGUMENT ||
        oracis_host_runtime_set_spi_firmware_image(
            synthetic_firmware, 0u) != ORACIS_HOST_RUNTIME_INVALID_ARGUMENT ||
        oracis_host_runtime_set_spi_firmware_image(
            synthetic_firmware, UINT32_C(0x01000001)) !=
            ORACIS_HOST_RUNTIME_INVALID_ARGUMENT ||
        oracis_host_runtime_set_spi_firmware_image(
            synthetic_firmware,
            (uint32_t)sizeof(synthetic_firmware)) != ORACIS_HOST_RUNTIME_OK) {
        fprintf(stderr, "C host bridge failed to validate/install user SPI firmware image\\n");
        return 1;
    }
    /* The machine owns its copy; caller storage is no longer needed. */
    synthetic_firmware[0] = 0u;

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

    /* Normal graphics mode remains the compositor path for both engines. */
    bus_write_u32(UINT32_C(0x04000000), UINT32_C(1) << 16u);
    bus_write_u32(UINT32_C(0x04001000), UINT32_C(1) << 16u);
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


    /*
     * The SDL desktop executable and Android shared Activity both call this
     * same C scanline bridge. Exercise the complete 48-line BG0 3D lookahead
     * through that ABI using a synthetic opaque rear-plane color.
     */
    bus_write_u32(UINT32_C(0x04000350), UINT32_C(0x001F001F));
    bus_write_u32(
        UINT32_C(0x04000000),
        (UINT32_C(1) << 16u) | UINT32_C(0x00000108));
    for (uint32_t line = 0u; line < 192u; ++line) {
        if (oracis_host_runtime_render_scanline(
                0u, line, main_scanline, 256u, &video_report) !=
                ORACIS_HOST_RUNTIME_OK ||
            video_report.status != 0u ||
            video_report.three_d_raster_status != 0u ||
            main_scanline[0] != UINT16_C(0x001F) ||
            main_scanline[255] != UINT16_C(0x001F)) {
            fprintf(stderr,
                "C host bridge failed BG0 3D scanline %u "
                "(status=%u raster=%u first=%04X last=%04X)\\n",
                line, video_report.status, video_report.three_d_raster_status,
                main_scanline[0], main_scanline[255]);
            return 1;
        }
    }
    if ((bus_read_u16(UINT32_C(0x04000060)) &
            UINT16_C(1u << 12u)) != 0u) {
        fprintf(stderr, "C host bridge raised BG0 3D underflow during a complete synthetic frame\\n");
        return 1;
    }
    /*
     * Rendering the next host frame starts again at line zero after the
     * previous consumer reached line 192. The host-side recovery must rebuild
     * the lookahead instead of treating the exhausted FIFO as an underrun.
     */
    for (uint32_t line = 0u; line < 192u; ++line) {
        if (oracis_host_runtime_render_scanline(
                0u, line, main_scanline, 256u, &video_report) !=
                ORACIS_HOST_RUNTIME_OK ||
            video_report.status != 0u ||
            video_report.three_d_raster_status != 0u ||
            main_scanline[0] != UINT16_C(0x001F) ||
            main_scanline[255] != UINT16_C(0x001F)) {
            fprintf(stderr,
                "C host bridge failed BG0 3D replay frame at scanline %u "
                "(status=%u raster=%u)\\n",
                line, video_report.status, video_report.three_d_raster_status);
            return 1;
        }
    }
    if ((bus_read_u16(UINT32_C(0x04000060)) &
            UINT16_C(1u << 12u)) != 0u) {
        fprintf(stderr, "C host bridge raised BG0 3D underflow during consecutive synthetic frames\\n");
        return 1;
    }
    /* Leave later tests on the ordinary 2D compositor path. */
    bus_write_u32(UINT32_C(0x04000000), UINT32_C(1) << 16u);

    /*
     * The generated-bank C bus must route GX command ports to the Geometry
     * Engine before the broader display-register device claims 0x0400xxxx.
     */
    bus_write_u32(UINT32_C(0x04000400), UINT32_C(0x10)); /* MTX_MODE */
    bus_write_u32(UINT32_C(0x04000400), UINT32_C(1));    /* Position matrix */
    const uint32_t gx_status = bus_read_u32(UINT32_C(0x04000600));
    if (((gx_status >> 16u) & UINT32_C(0x1FF)) != 0u ||
        (gx_status & (UINT32_C(1) << 26u)) == 0u ||
        (gx_status & (UINT32_C(1) << 27u)) == 0u) {
        fprintf(stderr, "C host bus did not deliver GXFIFO writes to the four-entry PIPE\\n");
        return 1;
    }

    /* MASTER_BRIGHT is applied after the ordinary BG/OBJ compositor. */
    bus_write_u16(UINT32_C(0x0400006C), UINT16_C(0x4010));
    bus_write_u16(UINT32_C(0x0400106C), UINT16_C(0x4010));
    if (oracis_host_runtime_render_scanline(
            0u, 0u, main_scanline, 256u, &video_report) !=
            ORACIS_HOST_RUNTIME_OK ||
        main_scanline[0] != UINT16_C(0x7FFF) ||
        oracis_host_runtime_render_scanline(
            1u, 191u, sub_scanline, 256u, &video_report) !=
            ORACIS_HOST_RUNTIME_OK ||
        sub_scanline[255] != UINT16_C(0x7FFF)) {
        fprintf(stderr, "C host bridge did not apply engine-specific MASTER_BRIGHT after composition\n");
        return 1;
    }
    bus_write_u16(UINT32_C(0x0400006C), UINT16_C(0));
    bus_write_u16(UINT32_C(0x0400106C), UINT16_C(0));

    /* DISPCNT mode 2 selects a raw RGB555 image from VRAM A-D. */
    bus_write_u8(UINT32_C(0x04000240), UINT8_C(0x80)); /* VRAM A: LCDC */
    bus_write_u8(UINT32_C(0x04000241), UINT8_C(0x80)); /* VRAM B: LCDC */
    bus_write_u16(UINT32_C(0x06800000), UINT16_C(0x9234));
    bus_write_u16(UINT32_C(0x06800002), UINT16_C(0x4567));
    bus_write_u16(UINT32_C(0x06800000) + UINT32_C(256) * 2u,
        UINT16_C(0x2ABC));
    bus_write_u16(UINT32_C(0x06820000), UINT16_C(0x1357));

    bus_write_u32(UINT32_C(0x04000000), UINT32_C(2) << 16u);
    if (oracis_host_runtime_render_scanline(
            0u, 0u, main_scanline, 256u, &video_report) !=
            ORACIS_HOST_RUNTIME_OK ||
        main_scanline[0] != UINT16_C(0x1234) ||
        main_scanline[1] != UINT16_C(0x4567)) {
        fprintf(stderr, "C host bridge failed to display VRAM A RGB555 data\n");
        return 1;
    }
    if (oracis_host_runtime_render_scanline(
            0u, 1u, main_scanline, 256u, &video_report) !=
            ORACIS_HOST_RUNTIME_OK ||
        main_scanline[0] != UINT16_C(0x2ABC)) {
        fprintf(stderr, "C host bridge used the wrong VRAM display scanline offset\n");
        return 1;
    }

    /* DISPCNT.18-19 switches the source bank from A to B. */
    bus_write_u32(UINT32_C(0x04000000),
        (UINT32_C(2) << 16u) | (UINT32_C(1) << 18u));
    if (oracis_host_runtime_render_scanline(
            0u, 0u, main_scanline, 256u, &video_report) !=
            ORACIS_HOST_RUNTIME_OK ||
        main_scanline[0] != UINT16_C(0x1357)) {
        fprintf(stderr, "C host bridge ignored the selected VRAM display bank\n");
        return 1;
    }

    /* Master brightness also applies to the direct VRAM display path. */
    bus_write_u16(UINT32_C(0x0400006C), UINT16_C(0x8010));
    if (oracis_host_runtime_render_scanline(
            0u, 0u, main_scanline, 256u, &video_report) !=
            ORACIS_HOST_RUNTIME_OK ||
        main_scanline[0] != UINT16_C(0)) {
        fprintf(stderr, "C host bridge skipped MASTER_BRIGHT in direct VRAM mode\n");
        return 1;
    }
    bus_write_u16(UINT32_C(0x0400006C), UINT16_C(0));

    /* An unallocated source bank fails closed and leaves caller pixels intact. */
    bus_write_u8(UINT32_C(0x04000241), 0u);
    for (unsigned x = 0u; x < 256u; ++x) main_scanline[x] = UINT16_C(0x4321);
    if (oracis_host_runtime_render_scanline(
            0u, 0u, main_scanline, 256u, &video_report) !=
            ORACIS_HOST_RUNTIME_VIDEO_ERROR ||
        video_report.status == 0u ||
        video_report.memory_status == 0u ||
        main_scanline[0] != UINT16_C(0x4321) ||
        main_scanline[255] != UINT16_C(0x4321)) {
        fprintf(stderr, "C host bridge accepted an unallocated VRAM display bank or partially wrote output\n");
        return 1;
    }

    /* Display-off white is also fed through each engine's master stage. */
    bus_write_u32(UINT32_C(0x04000000), 0u);
    bus_write_u32(UINT32_C(0x04001000), 0u);
    bus_write_u16(UINT32_C(0x0400006C), UINT16_C(0x8010));
    bus_write_u16(UINT32_C(0x0400106C), UINT16_C(0x8010));
    if (oracis_host_runtime_render_scanline(
            0u, 0u, main_scanline, 256u, &video_report) !=
            ORACIS_HOST_RUNTIME_OK ||
        main_scanline[0] != UINT16_C(0) ||
        main_scanline[255] != UINT16_C(0) ||
        oracis_host_runtime_render_scanline(
            1u, 191u, sub_scanline, 256u, &video_report) !=
            ORACIS_HOST_RUNTIME_OK ||
        sub_scanline[0] != UINT16_C(0) ||
        sub_scanline[255] != UINT16_C(0)) {
        fprintf(stderr, "C host bridge did not apply MASTER_BRIGHT to display-off output\n");
        return 1;
    }
    bus_write_u16(UINT32_C(0x0400006C), UINT16_C(0));
    bus_write_u16(UINT32_C(0x0400106C), UINT16_C(0));
    if (oracis_host_runtime_render_scanline(
            0u, 0u, main_scanline, 256u, &video_report) !=
            ORACIS_HOST_RUNTIME_OK ||
        main_scanline[0] != UINT16_C(0x7FFF) ||
        main_scanline[255] != UINT16_C(0x7FFF) ||
        oracis_host_runtime_render_scanline(
            1u, 191u, sub_scanline, 256u, &video_report) !=
            ORACIS_HOST_RUNTIME_OK ||
        sub_scanline[0] != UINT16_C(0x7FFF) ||
        sub_scanline[255] != UINT16_C(0x7FFF)) {
        fprintf(stderr, "C host bridge did not produce white output in display-off mode\n");
        return 1;
    }

    /* Release the test banks and restore ordinary graphics mode. */
    bus_write_u8(UINT32_C(0x04000240), 0u);
    bus_write_u32(UINT32_C(0x04000000), UINT32_C(1) << 16u);
    bus_write_u32(UINT32_C(0x04001000), UINT32_C(1) << 16u);

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
    if (oracis_host_runtime_register_global_destructor(
            (uint32_t)NDS_ARM9, UINT32_C(0x02001000),
            UINT32_C(0x02001020), UINT32_C(0x02001031),
            UINT32_C(0x02001040)) != ORACIS_HOST_RUNTIME_OVERLAY_BUSY ||
        oracis_host_runtime_register_active_global_destructor(
            UINT32_C(0x02001000)) != ORACIS_HOST_RUNTIME_OVERLAY_BUSY) {
        fprintf(stderr, "C destructor bridge accepted a host-boundary registration\\n");
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

    /*
     * run_round must apply the same live-byte preflight automatically. A
     * stale ARM7 entry must be rejected before either CPU executes or the
     * global bus clock advances.
     */
    if (oracis_host_runtime_run_round(1u, NULL, NULL) !=
            ORACIS_HOST_RUNTIME_ENTRYPOINT_UNRESOLVED ||
        oracis_host_runtime_global_bus_cycles() != UINT64_C(0) ||
        g_arm9_bank_calls != 0u || g_arm7_bank_calls != 0u) {
        bus_write_u16(UINT32_C(0x02381000), UINT16_C(0x4770));
        (void)oracis_arm_runtime_select_cpu(NDS_ARM9);
        fprintf(stderr,
            "host bridge executed a round with stale ARM7 entry bytes\\n");
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
    if (oracis_host_runtime_run_round(1u, NULL, NULL) !=
            ORACIS_HOST_RUNTIME_ENTRYPOINT_UNRESOLVED ||
        oracis_host_runtime_global_bus_cycles() != UINT64_C(0) ||
        g_arm9_bank_calls != 0u || g_arm7_bank_calls != 0u) {
        oracis_active_cpu_state->cpsr |= CPSR_T_BIT;
        (void)oracis_arm_runtime_select_cpu(NDS_ARM9);
        fprintf(stderr,
            "host bridge executed ARM7 after entry mode stopped matching CPSR.T\\n");
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

    bus_write_u32(TEST_DTOR_HEAD, 0u);
    bus_write_u32(TEST_DTOR_NODE, UINT32_C(0xAAAAAAAA));
    bus_write_u32(TEST_DTOR_NODE + 4u, UINT32_C(0xBBBBBBBB));
    bus_write_u32(TEST_DTOR_NODE + 8u, UINT32_C(0xCCCCCCCC));
    g_request_active_destructor_registration = 1;
    g_active_destructor_registration_status = ORACIS_HOST_RUNTIME_INVALID_ARGUMENT;

    const int irq_round_status =
        oracis_host_runtime_run_round(1u, NULL, NULL);
    const uint64_t irq_round_bus_cycles =
        oracis_host_runtime_global_bus_cycles();
    const uint32_t registered_head = bus_read_u32(TEST_DTOR_HEAD);
    const uint32_t registered_next = bus_read_u32(TEST_DTOR_NODE);
    const uint32_t registered_dtor = bus_read_u32(TEST_DTOR_NODE + 4u);
    const uint32_t registered_obj = bus_read_u32(TEST_DTOR_NODE + 8u);
    if (irq_round_status != ORACIS_HOST_RUNTIME_OK ||
        g_arm9_bank_calls != 2u ||
        g_arm7_bank_calls != 1u ||
        g_arm9_irq_handler_ok != 1 ||
        g_arm7_irq_handler_ok != 1 ||
        irq_round_bus_cycles != UINT64_C(1) ||
        g_request_active_destructor_registration != 0 ||
        g_active_destructor_registration_status != ORACIS_HOST_RUNTIME_OK ||
        registered_head != TEST_DTOR_NODE ||
        registered_next != 0u ||
        registered_dtor != TEST_DTOR_FUNCTION ||
        registered_obj != TEST_DTOR_OBJECT) {
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

    /*
     * Add short-lived C tables while the machine is running. The helper shuts
     * down the host before returning, so both its tables and the tables above
     * are removed while their backing storage is still alive.
     */
    if (oracis_host_runtime_has_program_banks() != 1 ||
        shutdown_with_temporary_dispatch_tables() != 0 ||
        oracis_host_runtime_is_initialized() != 0 ||
        oracis_host_runtime_has_program_banks() != 0 ||
        oracis_host_runtime_last_machine_status() != 0) {
        fprintf(stderr, "C host bridge failed to clear dispatch tables at shutdown\n");
        return 1;
    }

    if (oracis_host_runtime_shutdown() != ORACIS_HOST_RUNTIME_OK) {
        fprintf(stderr, "C host bridge shutdown was not idempotent\n");
        return 1;
    }

    /* The helper's table storage is now out of scope. Neither CPU may retain
       a registry entry or return a pointer into those expired stack arrays. */
    uint32_t arm9_lookup_status = UINT32_MAX;
    uint32_t arm7_lookup_status = UINT32_MAX;
    uint32_t arm9_candidate_count = UINT32_MAX;
    uint32_t arm7_candidate_count = UINT32_MAX;
    const NdsDispatchEntry *arm9_candidate = NULL;
    const NdsDispatchEntry *arm7_candidate = NULL;
    if (oracis_static_dispatch_lookup(
            NDS_ARM9, UINT32_C(0x02004000), 0u,
            &arm9_lookup_status, &arm9_candidate_count,
            &arm9_candidate) != NULL ||
        arm9_lookup_status != ORACIS_DISPATCH_LOOKUP_CPU_NOT_REGISTERED ||
        arm9_candidate_count != 0u || arm9_candidate != NULL ||
        oracis_static_dispatch_lookup(
            NDS_ARM7, UINT32_C(0x02384000), 0u,
            &arm7_lookup_status, &arm7_candidate_count,
            &arm7_candidate) != NULL ||
        arm7_lookup_status != ORACIS_DISPATCH_LOOKUP_CPU_NOT_REGISTERED ||
        arm7_candidate_count != 0u || arm7_candidate != NULL) {
        fprintf(stderr, "C host bridge retained expired temporary dispatch tables\n");
        return 1;
    }

    /* Host shutdown must release the previous device map so this process can
       start a completely new machine and register a fresh set of banks. */
    if (oracis_host_runtime_initialize() != ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_is_initialized() != 1 ||
        oracis_host_runtime_has_program_banks() != 0 ||
        oracis_host_runtime_run_round(1u, NULL, NULL) !=
            ORACIS_HOST_RUNTIME_NO_PROGRAM_BANKS ||
        oracis_host_runtime_shutdown() != ORACIS_HOST_RUNTIME_OK) {
        fprintf(stderr, "C host bridge could not initialize a clean second machine lifecycle\n");
        return 1;
    }

    oracis_host_set_keypad_state(0u);
    puts("C host runtime bridge lifecycle/keypad/2D scanline/ARM9-ARM7 IRQ tests passed");
    return 0;
}
