#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_iostream.h>

#include "oracis/memory/guest_memory_abi.h"
#include "oracis/host/runtime_bridge.h"

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ORACIS_SCREEN_WIDTH = 256,
    ORACIS_SCREEN_HEIGHT = 192,
    ORACIS_SCREEN_GAP = 8,
    ORACIS_LOGICAL_HEIGHT = (ORACIS_SCREEN_HEIGHT * 2) + ORACIS_SCREEN_GAP
};

enum {
    ORACIS_KEY_A      = 1u << 0u,
    ORACIS_KEY_B      = 1u << 1u,
    ORACIS_KEY_SELECT = 1u << 2u,
    ORACIS_KEY_START  = 1u << 3u,
    ORACIS_KEY_RIGHT  = 1u << 4u,
    ORACIS_KEY_LEFT   = 1u << 5u,
    ORACIS_KEY_UP     = 1u << 6u,
    ORACIS_KEY_DOWN   = 1u << 7u,
    ORACIS_KEY_R      = 1u << 8u,
    ORACIS_KEY_L      = 1u << 9u
};

typedef struct OracisKeyBinding {
    SDL_Keycode key;
    uint16_t mask;
} OracisKeyBinding;

/*
 * Desktop defaults:
 *   X/Z = A/B, Enter/Backspace = Start/Select,
 *   arrows = D-pad, Q/W = L/R.
 */
static const OracisKeyBinding ORACIS_KEY_BINDINGS[] = {
    { SDLK_X, ORACIS_KEY_A },
    { SDLK_Z, ORACIS_KEY_B },
    { SDLK_BACKSPACE, ORACIS_KEY_SELECT },
    { SDLK_RETURN, ORACIS_KEY_START },
    { SDLK_RIGHT, ORACIS_KEY_RIGHT },
    { SDLK_LEFT, ORACIS_KEY_LEFT },
    { SDLK_UP, ORACIS_KEY_UP },
    { SDLK_DOWN, ORACIS_KEY_DOWN },
    { SDLK_W, ORACIS_KEY_R },
    { SDLK_Q, ORACIS_KEY_L }
};

static uint16_t g_pressed_keys = 0u;

static void update_keypad_key(SDL_Keycode key, bool pressed)
{
    const size_t binding_count =
        sizeof(ORACIS_KEY_BINDINGS) / sizeof(ORACIS_KEY_BINDINGS[0]);

    for (size_t index = 0u; index < binding_count; ++index) {
        if (ORACIS_KEY_BINDINGS[index].key == key) {
            const uint16_t mask = ORACIS_KEY_BINDINGS[index].mask;
            const uint16_t previous = g_pressed_keys;

            if (pressed) {
                g_pressed_keys = (uint16_t)(g_pressed_keys | mask);
            } else {
                g_pressed_keys = (uint16_t)(g_pressed_keys & (uint16_t)~mask);
            }

            if (g_pressed_keys != previous) {
                oracis_host_set_keypad_state(g_pressed_keys);
            }
            return;
        }
    }
}

static void release_all_keys(void)
{
    if (g_pressed_keys != 0u) {
        g_pressed_keys = 0u;
        oracis_host_set_keypad_state(0u);
    }
}

static bool g_video_error_reported = false;

static bool parse_game_card_chip_id(const char *text, uint32_t *chip_id)
{
    if (text == NULL || text[0] == '\0' || text[0] == '-') return false;
    errno = 0;
    char *end = NULL;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (errno == ERANGE || end == text || end == NULL || *end != '\0' ||
        parsed > UINT32_MAX || chip_id == NULL) {
        return false;
    }
    *chip_id = (uint32_t)parsed;
    return true;
}

static bool read_exact_io(SDL_IOStream *stream, void *buffer, size_t size)
{
    size_t offset = 0u;
    uint8_t *bytes = (uint8_t *)buffer;
    while (offset < size) {
        const size_t count = SDL_ReadIO(stream, bytes + offset, size - offset);
        if (count == 0u) return false;
        offset += count;
    }
    return true;
}

static bool mount_game_card_file(const char *path)
{
    SDL_IOStream *file = SDL_IOFromFile(path, "rb");
    if (file == NULL) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "Could not open Game Card image '%s': %s", path, SDL_GetError());
        return false;
    }

    const Sint64 file_size = SDL_GetIOSize(file);
    if (file_size < 0x200 || (Uint64)file_size > UINT64_C(0x20000000)) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "Game Card image '%s' has an invalid size (%lld bytes); "
            "expected 512 bytes through 512 MiB.",
            path, (long long)file_size);
        (void)SDL_CloseIO(file);
        return false;
    }

    if (SDL_SeekIO(file, 0, SDL_IO_SEEK_SET) < 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "Could not rewind Game Card image '%s': %s", path, SDL_GetError());
        (void)SDL_CloseIO(file);
        return false;
    }

    const size_t image_size = (size_t)file_size;
    void *image = malloc(image_size);
    if (image == NULL) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "Could not allocate %zu bytes for Game Card image '%s'.",
            image_size, path);
        (void)SDL_CloseIO(file);
        return false;
    }

    const bool read_ok = read_exact_io(file, image, image_size);
    const bool close_ok = SDL_CloseIO(file);
    if (!read_ok || !close_ok) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "Could not read the complete Game Card image '%s': %s",
            path, SDL_GetError());
        free(image);
        return false;
    }

    const int status = oracis_host_runtime_mount_game_card_image(
        (const uint8_t *)image, (uint32_t)image_size);
    free(image);
    if (status != ORACIS_HOST_RUNTIME_OK) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "Game Card image mount failed (bridge=%d).", status);
        return false;
    }

    SDL_Log("Mounted user Game Card image '%s' (%zu bytes). "
            "KEY1/KEY2 command execution is still incomplete.",
            path, image_size);
    return true;
}

static bool load_arm7_bios_file(const char *path)
{
    SDL_IOStream *file = SDL_IOFromFile(path, "rb");
    if (file == NULL) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "Could not open NDS ARM7 BIOS '%s': %s", path, SDL_GetError());
        return false;
    }

    const Sint64 file_size = SDL_GetIOSize(file);
    if (file_size != 0x4000 ||
        SDL_SeekIO(file, 0, SDL_IO_SEEK_SET) < 0) {
        (void)SDL_CloseIO(file);
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "NDS ARM7 BIOS '%s' must be exactly 16384 readable bytes.", path);
        return false;
    }

    uint8_t *bytes = (uint8_t *)malloc(0x4000u);
    if (bytes == NULL) {
        (void)SDL_CloseIO(file);
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "Could not allocate memory for NDS ARM7 BIOS '%s'.", path);
        return false;
    }

    const bool read_ok = read_exact_io(file, bytes, 0x4000u);
    const bool close_ok = SDL_CloseIO(file);
    if (!read_ok || !close_ok) {
        free(bytes);
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "Could not read the complete NDS ARM7 BIOS '%s': %s",
            path, SDL_GetError());
        return false;
    }

    const int status = oracis_host_runtime_load_game_card_arm7_bios(
        bytes, 0x4000u);
    free(bytes);
    if (status != ORACIS_HOST_RUNTIME_OK) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "Could not initialize the Game Card KEY1 table (bridge=%d).",
            status);
        return false;
    }

    SDL_Log("Loaded the KEY1 table from user-supplied ARM7 BIOS '%s'.", path);
    return true;
}

static bool load_firmware_file(const char *path)
{
    SDL_IOStream *file = SDL_IOFromFile(path, "rb");
    if (file == NULL) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "Could not open user NDS firmware image '%s': %s",
            path, SDL_GetError());
        return false;
    }

    const Sint64 file_size = SDL_GetIOSize(file);
    if (file_size <= 0 || file_size > INT64_C(0x01000000) ||
        SDL_SeekIO(file, 0, SDL_IO_SEEK_SET) < 0) {
        (void)SDL_CloseIO(file);
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "NDS firmware image '%s' must contain between 1 byte and 16 MiB.",
            path);
        return false;
    }

    const size_t image_size = (size_t)file_size;
    uint8_t *bytes = (uint8_t *)malloc(image_size);
    if (bytes == NULL) {
        (void)SDL_CloseIO(file);
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "Could not allocate %zu bytes for NDS firmware image '%s'.",
            image_size, path);
        return false;
    }

    const bool read_ok = read_exact_io(file, bytes, image_size);
    const bool close_ok = SDL_CloseIO(file);
    if (!read_ok || !close_ok) {
        free(bytes);
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "Could not read the complete NDS firmware image '%s': %s",
            path, SDL_GetError());
        return false;
    }

    const int status = oracis_host_runtime_set_spi_firmware_image(
        bytes, (uint32_t)image_size);
    free(bytes);
    if (status != ORACIS_HOST_RUNTIME_OK) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "Could not install user SPI firmware image (bridge=%d).",
            status);
        return false;
    }

    SDL_Log("Loaded user-provided NDS SPI firmware flash '%s' (%zu bytes). "
            "Firmware IPL code decryption/decompression is not yet implemented.",
            path, image_size);
    return true;
}

#ifdef __ANDROID__
enum {
    ORACIS_ANDROID_FILE_GAME_CARD = 1,
    ORACIS_ANDROID_FILE_ARM7_BIOS = 2,
    ORACIS_ANDROID_FILE_FIRMWARE = 3,
    ORACIS_ANDROID_DIALOG_WAITING = 1,
    ORACIS_ANDROID_DIALOG_SELECTED = 2,
    ORACIS_ANDROID_DIALOG_CANCELLED = 3,
    ORACIS_ANDROID_DIALOG_FAILED = 4
};

static SDL_Mutex *g_android_dialog_mutex = NULL;
static int g_android_dialog_state = 0;
static int g_android_dialog_kind = 0;
static char *g_android_dialog_path = NULL;
static char *g_android_game_card_uri = NULL;
static char *g_android_arm7_bios_uri = NULL;
static char *g_android_firmware_uri = NULL;
static bool g_android_guest_execution_enabled = false;
static bool g_android_guest_stacks_prepared = false;
static uint64_t g_android_last_cycle_report = 0u;

static void SDLCALL android_file_dialog_callback(
    void *userdata, const char * const *filelist, int filter)
{
    const int kind = (int)(intptr_t)userdata;
    char *selected_path = NULL;
    int result = ORACIS_ANDROID_DIALOG_FAILED;
    (void)filter;

    if (filelist != NULL) {
        if (filelist[0] == NULL) {
            result = ORACIS_ANDROID_DIALOG_CANCELLED;
        } else if (filelist[1] == NULL) {
            selected_path = SDL_strdup(filelist[0]);
            result = selected_path != NULL
                ? ORACIS_ANDROID_DIALOG_SELECTED
                : ORACIS_ANDROID_DIALOG_FAILED;
        }
    }

    if (g_android_dialog_mutex == NULL) {
        SDL_free(selected_path);
        return;
    }

    SDL_LockMutex(g_android_dialog_mutex);
    SDL_free(g_android_dialog_path);
    g_android_dialog_path = selected_path;
    g_android_dialog_kind = kind;
    g_android_dialog_state = result;
    SDL_UnlockMutex(g_android_dialog_mutex);
}

static bool begin_android_file_dialog(SDL_Window *window, int kind)
{
    static const SDL_DialogFileFilter game_card_filter[] = {
        { "Nintendo DS game image", "nds" }
    };
    static const SDL_DialogFileFilter arm7_bios_filter[] = {
        { "Nintendo DS ARM7 BIOS", "bin" }
    };
    static const SDL_DialogFileFilter firmware_filter[] = {
        { "Nintendo DS firmware flash image", "bin" }
    };

    if (g_android_dialog_mutex == NULL) {
        g_android_dialog_mutex = SDL_CreateMutex();
        if (g_android_dialog_mutex == NULL) {
            SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                "Could not create Android file-dialog synchronization: %s",
                SDL_GetError());
            return false;
        }
    }

    SDL_LockMutex(g_android_dialog_mutex);
    if (g_android_dialog_state == ORACIS_ANDROID_DIALOG_WAITING) {
        SDL_UnlockMutex(g_android_dialog_mutex);
        return false;
    }
    g_android_dialog_state = ORACIS_ANDROID_DIALOG_WAITING;
    g_android_dialog_kind = kind;
    SDL_free(g_android_dialog_path);
    g_android_dialog_path = NULL;
    SDL_UnlockMutex(g_android_dialog_mutex);

    if (kind == ORACIS_ANDROID_FILE_GAME_CARD) {
        SDL_Log("Choose your own Pokémon Platinum USA Rev 1 .nds image.");
        SDL_ShowOpenFileDialog(
            android_file_dialog_callback, (void *)(intptr_t)kind,
            window, game_card_filter, 1, NULL, false);
    } else if (kind == ORACIS_ANDROID_FILE_ARM7_BIOS) {
        SDL_Log("Choose your own 16 KiB Nintendo DS ARM7 BIOS (bios7.bin).");
        SDL_ShowOpenFileDialog(
            android_file_dialog_callback, (void *)(intptr_t)kind,
            window, arm7_bios_filter, 1, NULL, false);
    } else {
        SDL_Log("Choose your own Nintendo DS SPI firmware flash dump (1..16 MiB).");
        SDL_ShowOpenFileDialog(
            android_file_dialog_callback, (void *)(intptr_t)kind,
            window, firmware_filter, 1, NULL, false);
    }
    return true;
}

static bool take_android_file_dialog_result(
    int *kind, int *result, char **path)
{
    bool ready = false;
    if (g_android_dialog_mutex == NULL ||
        kind == NULL || result == NULL || path == NULL) {
        return false;
    }

    SDL_LockMutex(g_android_dialog_mutex);
    if (g_android_dialog_state != ORACIS_ANDROID_DIALOG_WAITING &&
        g_android_dialog_state != 0) {
        *kind = g_android_dialog_kind;
        *result = g_android_dialog_state;
        *path = g_android_dialog_path;
        g_android_dialog_path = NULL;
        g_android_dialog_state = 0;
        g_android_dialog_kind = 0;
        ready = true;
    }
    SDL_UnlockMutex(g_android_dialog_mutex);
    return ready;
}

static void log_android_entrypoint_preflight(void)
{
    OracisHostRuntimeEntryPointReport arm9 = {0};
    OracisHostRuntimeEntryPointReport arm7 = {0};
    const int status = oracis_host_runtime_validate_entrypoints(
        &arm9, &arm7);

    if (status != ORACIS_HOST_RUNTIME_OK) {
        g_android_guest_execution_enabled = false;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Android static-entry preflight unavailable (bridge=%d, machine=%d).",
            status, oracis_host_runtime_last_machine_status());
        return;
    }

    SDL_Log("Android entry preflight: ARM9 PC=0x%08X resolved=%u "
            "(lookup=%u candidates=%u inactive=%u); "
            "ARM7 PC=0x%08X resolved=%u "
            "(lookup=%u candidates=%u inactive=%u).",
            arm9.pc, arm9.resolved, arm9.lookup_status,
            arm9.candidate_count, arm9.has_inactive_candidate,
            arm7.pc, arm7.resolved, arm7.lookup_status,
            arm7.candidate_count, arm7.has_inactive_candidate);

    g_android_guest_execution_enabled =
        arm9.resolved != 0u && arm7.resolved != 0u;
    if (g_android_guest_execution_enabled) {
        g_android_last_cycle_report = oracis_host_runtime_global_bus_cycles();
        SDL_Log("Both native entry PCs resolve to registered static banks. "
                "The machine loader has seeded the documented cartridge-entry "
                "register/stack state and POSTFLG. BIOS/firmware power-on is "
                "still bypassed; compatibility is not yet established.");
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "Native direct-entry remains disabled because one or both entry PCs "
            "do not resolve to compiled static banks.");
    }
}
#endif

static uint8_t expand_rgb555_channel(uint16_t channel)
{
    return (uint8_t)((channel << 3u) | (channel >> 2u));
}

static bool update_composed_screen(
    SDL_Texture *texture,
    uint32_t engine_index,
    uint32_t *pixels)
{
    uint16_t rgb555[ORACIS_SCREEN_WIDTH];

    for (uint32_t scanline = 0u;
         scanline < ORACIS_SCREEN_HEIGHT;
         ++scanline) {
        OracisHostVideoScanlineReport report = {0};
        const int status = oracis_host_runtime_render_scanline(
            engine_index,
            scanline,
            rgb555,
            ORACIS_SCREEN_WIDTH,
            &report);
        if (status != ORACIS_HOST_RUNTIME_OK) {
            if (!g_video_error_reported) {
                SDL_LogWarn(
                    SDL_LOG_CATEGORY_APPLICATION,
                    "NDS 2D composition failed (engine=%u, line=%u, "
                    "bridge=%d, compositor=%u, BG=%u, fetch=%u, memory=%u, "
                    "OBJ=%u, OBJ index=%u, 3D=%u). The frame was not presented.",
                    engine_index,
                    scanline,
                    status,
                    report.status,
                    report.background_index,
                    report.fetch_status,
                    report.memory_status,
                    report.object_fetch_status,
                    report.object_index,
                    report.three_d_raster_status);
                g_video_error_reported = true;
            }
            return false;
        }

        const size_t row_offset =
            (size_t)scanline * (size_t)ORACIS_SCREEN_WIDTH;
        for (uint32_t x = 0u; x < ORACIS_SCREEN_WIDTH; ++x) {
            const uint16_t color = rgb555[x];
            const uint32_t red = expand_rgb555_channel(color & 0x1Fu);
            const uint32_t green =
                expand_rgb555_channel((color >> 5u) & 0x1Fu);
            const uint32_t blue =
                expand_rgb555_channel((color >> 10u) & 0x1Fu);
            pixels[row_offset + x] =
                UINT32_C(0xFF000000) | (red << 16u) |
                (green << 8u) | blue;
        }
    }

    if (!SDL_UpdateTexture(
            texture,
            NULL,
            pixels,
            ORACIS_SCREEN_WIDTH * (int)sizeof(*pixels))) {
        if (!g_video_error_reported) {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "Could not upload NDS screen texture: %s",
                SDL_GetError());
            g_video_error_reported = true;
        }
        return false;
    }

    return true;
}

static bool draw_composed_screens(
    SDL_Renderer *renderer,
    SDL_Texture *textures[2],
    uint32_t *pixels[2])
{
    const SDL_FRect top_screen = {
        0.0f,
        0.0f,
        (float)ORACIS_SCREEN_WIDTH,
        (float)ORACIS_SCREEN_HEIGHT
    };
    const SDL_FRect bottom_screen = {
        0.0f,
        (float)(ORACIS_SCREEN_HEIGHT + ORACIS_SCREEN_GAP),
        (float)ORACIS_SCREEN_WIDTH,
        (float)ORACIS_SCREEN_HEIGHT
    };

    if (!update_composed_screen(textures[0], 0u, pixels[0]) ||
        !update_composed_screen(textures[1], 1u, pixels[1])) {
        return false;
    }

    SDL_SetRenderDrawColor(renderer, 18, 20, 28, 255);
    if (!SDL_RenderClear(renderer) ||
        !SDL_RenderTexture(renderer, textures[0], NULL, &top_screen) ||
        !SDL_RenderTexture(renderer, textures[1], NULL, &bottom_screen)) {
        if (!g_video_error_reported) {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "Could not present NDS screen textures: %s",
                SDL_GetError());
            g_video_error_reported = true;
        }
        return false;
    }

    g_video_error_reported = false;
    return true;
}

int main(int argc, char *argv[])
{
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *screen_textures[2] = {NULL, NULL};
    uint32_t *screen_pixels[2] = {NULL, NULL};
    SDL_Event event;
    bool running = true;
    bool host_runtime_initialized = false;
    int exit_code = EXIT_FAILURE;
    const char *game_card_path = NULL;
    const char *arm7_bios_path = NULL;
    const char *firmware_path = NULL;
    uint32_t game_card_chip_id = 0u;
    bool game_card_chip_id_set = false;
    bool game_card_program_loaded = false;
    bool firmware_loaded = false;

    oracis_host_set_keypad_state(0u);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_LogCritical(
            SDL_LOG_CATEGORY_APPLICATION,
            "SDL initialization failed: %s",
            SDL_GetError()
        );
        return EXIT_FAILURE;
    }

    for (int argument = 1; argument < argc; ++argument) {
        if (strcmp(argv[argument], "--game-card") == 0) {
            if (argument + 1 >= argc || game_card_path != NULL) {
                SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                    "Usage: %s [--game-card path/to/game.nds] [--game-card-id 0xID]", argv[0]);
                goto cleanup;
            }
            game_card_path = argv[++argument];
        } else if (strcmp(argv[argument], "--arm7-bios") == 0) {
            if (argument + 1 >= argc || arm7_bios_path != NULL) {
                SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                    "--arm7-bios requires one path to a 16 KiB NDS ARM7 BIOS.");
                goto cleanup;
            }
            arm7_bios_path = argv[++argument];
        } else if (strcmp(argv[argument], "--firmware") == 0) {
            if (argument + 1 >= argc || firmware_path != NULL) {
                SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                    "--firmware requires one path to a user-provided NDS SPI firmware dump.");
                goto cleanup;
            }
            firmware_path = argv[++argument];
        } else if (strcmp(argv[argument], "--game-card-id") == 0) {
            if (argument + 1 >= argc || game_card_chip_id_set ||
                !parse_game_card_chip_id(argv[argument + 1], &game_card_chip_id)) {
                SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                    "--game-card-id requires one uint32 value (decimal or 0x-prefixed hex).");
                goto cleanup;
            }
            game_card_chip_id_set = true;
            ++argument;
        } else if (strcmp(argv[argument], "--help") == 0 ||
                   strcmp(argv[argument], "-h") == 0) {
            SDL_Log("Usage: %s [--game-card path/to/game.nds] [--arm7-bios path/to/bios7.bin] [--firmware path/to/firmware.bin] [--game-card-id 0xID]", argv[0]);
            exit_code = EXIT_SUCCESS;
            goto cleanup;
        } else {
            SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                "Unknown argument '%s'. Use --help for usage.", argv[argument]);
            goto cleanup;
        }
    }

    if (game_card_chip_id_set && game_card_path == NULL) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "--game-card-id requires --game-card path/to/game.nds.");
        goto cleanup;
    }

    if (arm7_bios_path != NULL && game_card_path == NULL) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "--arm7-bios requires --game-card path/to/game.nds.");
        goto cleanup;
    }
    if (firmware_path != NULL &&
        (game_card_path == NULL || arm7_bios_path == NULL)) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "--firmware requires both --game-card and --arm7-bios.");
        goto cleanup;
    }

    if (!SDL_CreateWindowAndRenderer(
            "Oracis — native host prototype",
            ORACIS_SCREEN_WIDTH * 2,
            ORACIS_LOGICAL_HEIGHT * 2,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
            &window,
            &renderer)) {
        SDL_LogCritical(
            SDL_LOG_CATEGORY_APPLICATION,
            "Window/renderer creation failed: %s",
            SDL_GetError()
        );
        goto cleanup;
    }

    if (!SDL_SetRenderLogicalPresentation(
            renderer,
            ORACIS_SCREEN_WIDTH,
            ORACIS_LOGICAL_HEIGHT,
            SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
        SDL_LogCritical(
            SDL_LOG_CATEGORY_APPLICATION,
            "Logical presentation setup failed: %s",
            SDL_GetError()
        );
        goto cleanup;
    }

    const int runtime_status = oracis_host_runtime_initialize();
    if (runtime_status != ORACIS_HOST_RUNTIME_OK) {
        SDL_LogCritical(
            SDL_LOG_CATEGORY_APPLICATION,
            "NDS machine initialization failed (bridge=%d, machine=%d).",
            runtime_status,
            oracis_host_runtime_last_machine_status()
        );
        goto cleanup;
    }
    host_runtime_initialized = true;

#ifdef __ANDROID__
    if (game_card_path == NULL &&
        !begin_android_file_dialog(window, ORACIS_ANDROID_FILE_GAME_CARD)) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "Could not start the Android Game Card picker.");
        goto cleanup;
    }
#endif

    if (game_card_path != NULL && !mount_game_card_file(game_card_path)) {
        goto cleanup;
    }
    if (arm7_bios_path != NULL && !load_arm7_bios_file(arm7_bios_path)) {
        goto cleanup;
    }
    if (firmware_path != NULL) {
        if (!load_firmware_file(firmware_path)) goto cleanup;
        firmware_loaded = true;
    }
#ifdef __ANDROID__
    if (game_card_path != NULL && arm7_bios_path == NULL) {
        if (!begin_android_file_dialog(window, ORACIS_ANDROID_FILE_ARM7_BIOS)) {
            SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                "Could not start the Android ARM7 BIOS picker.");
            goto cleanup;
        }
    } else if (game_card_path != NULL && arm7_bios_path != NULL &&
               !firmware_loaded) {
        if (!begin_android_file_dialog(window, ORACIS_ANDROID_FILE_FIRMWARE)) {
            SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                "Could not start the Android SPI firmware picker.");
            goto cleanup;
        }
    }
#endif

    if (game_card_chip_id_set) {
        const int chip_status = oracis_host_runtime_set_game_card_chip_id(
            game_card_chip_id);
        if (chip_status != ORACIS_HOST_RUNTIME_OK) {
            SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                "Could not configure the physical Game Card chip ID (bridge=%d).",
                chip_status);
            goto cleanup;
        }
        SDL_Log("Configured physical Game Card chip ID: 0x%08X. This value is supplied by the user, not inferred from the .nds image.",
            game_card_chip_id);
    }

    if (game_card_path != NULL && arm7_bios_path != NULL &&
        (firmware_loaded
#ifndef __ANDROID__
         || true
#endif
        )) {
        const int load_status =
            oracis_host_runtime_load_mounted_game_card_program();
        if (load_status != ORACIS_HOST_RUNTIME_OK) {
            SDL_LogCritical(
                SDL_LOG_CATEGORY_APPLICATION,
                "Game Card ARM9/ARM7 loading failed (bridge=%d, machine=%d).",
                load_status,
                oracis_host_runtime_last_machine_status());
            goto cleanup;
        }
        game_card_program_loaded = true;
        SDL_Log(
            "ARM9/ARM7 segments were validated against the mounted NDS header; "
            "the ARM9 Secure Area prefix was decrypted into guest RAM. "
            "SPI firmware flash is available=%s; firmware IPL decryption/"
            "decompression and BIOS startup are not implemented.",
            firmware_loaded ? "yes" : "no");
    }

    for (uint32_t engine = 0u; engine < 2u; ++engine) {
        screen_pixels[engine] = (uint32_t *)calloc(
            (size_t)ORACIS_SCREEN_WIDTH * (size_t)ORACIS_SCREEN_HEIGHT,
            sizeof(uint32_t));
        screen_textures[engine] = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING,
            ORACIS_SCREEN_WIDTH,
            ORACIS_SCREEN_HEIGHT);
        if (screen_pixels[engine] == NULL ||
            screen_textures[engine] == NULL ||
            !SDL_SetTextureScaleMode(
                screen_textures[engine], SDL_SCALEMODE_NEAREST) ||
            !SDL_SetTextureBlendMode(
                screen_textures[engine], SDL_BLENDMODE_NONE)) {
            SDL_LogCritical(
                SDL_LOG_CATEGORY_APPLICATION,
                "NDS screen %u texture setup failed: %s",
                engine,
                SDL_GetError());
            goto cleanup;
        }
    }

#ifdef ORACIS_WITH_PLATINUM_STATIC_BANKS
    const int bank_status = oracis_host_runtime_register_compiled_banks();
    if (bank_status != ORACIS_HOST_RUNTIME_OK) {
        SDL_LogCritical(
            SDL_LOG_CATEGORY_APPLICATION,
            "Compiled ARM9/ARM7 bank registration failed (bridge=%d, machine=%d).",
            bank_status,
            oracis_host_runtime_last_machine_status()
        );
        goto cleanup;
    }
#if !defined(__ANDROID__)
    int image_status = ORACIS_HOST_RUNTIME_OK;
    if (!game_card_program_loaded && game_card_path == NULL) {
        image_status = oracis_host_runtime_load_local_platinum_segments(NULL);
    } else if (!game_card_program_loaded) {
        image_status = ORACIS_HOST_RUNTIME_LOCAL_IMAGE_MISSING;
    }
    if (image_status == ORACIS_HOST_RUNTIME_OK) {
        OracisHostRuntimeEntryPointReport arm9_entry = {0};
        OracisHostRuntimeEntryPointReport arm7_entry = {0};
        const int preflight_status = oracis_host_runtime_validate_entrypoints(
            &arm9_entry, &arm7_entry);
        if (preflight_status == ORACIS_HOST_RUNTIME_OK) {
            SDL_Log(
                "Static entry preflight: ARM9 PC=0x%08X resolved=%s "
                "(lookup=%u candidates=%u inactive=%u); ARM7 PC=0x%08X resolved=%s "
                "(lookup=%u candidates=%u inactive=%u). Execution remains disabled.",
                arm9_entry.pc, arm9_entry.resolved ? "yes" : "no",
                arm9_entry.lookup_status, arm9_entry.candidate_count,
                arm9_entry.has_inactive_candidate,
                arm7_entry.pc, arm7_entry.resolved ? "yes" : "no",
                arm7_entry.lookup_status, arm7_entry.candidate_count,
                arm7_entry.has_inactive_candidate);
        } else {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "Static entry preflight unavailable (bridge=%d).",
                preflight_status);
        }
        if (game_card_program_loaded) {
            SDL_Log(
                "User Game Card ARM9/ARM7 segments are in guest memory. "
                "Native execution remains disabled until BIOS startup, IRQ "
                "delivery and required runtime services are complete.");
        } else {
            SDL_Log(
                "Local Platinum USA Rev 1 ARM9/ARM7 images were size/hash verified "
                "and loaded into guest memory. Native execution remains disabled "
                "until the IRQ provider and required runtime services are complete."
            );
        }
    } else if (game_card_path != NULL) {
        SDL_Log(
            "Game Card image is mounted, but program loading was skipped because "
            "--arm7-bios was not supplied. Pass a user-owned 16 KiB ARM7 BIOS "
            "with --arm7-bios to validate and load the Secure Area.");
    } else if (image_status == ORACIS_HOST_RUNTIME_LOCAL_IMAGE_MISSING) {
        SDL_Log(
            "Local Platinum images were not found. The host remains in "
            "prototype mode; expected .oracis/local/pokemon-platinum-usa-rev1."
        );
    } else {
        SDL_LogCritical(
            SDL_LOG_CATEGORY_APPLICATION,
            "Local Platinum image loading failed (bridge=%d, machine=%d).",
            image_status,
            oracis_host_runtime_last_machine_status()
        );
        goto cleanup;
    }
#else
    if (game_card_program_loaded) {
        log_android_entrypoint_preflight();
        SDL_Log(
            "Android ROM/BIOS data loaded and segment identity validated. "
            "Native execution remains gated until BIOS-equivalent handoff "
            "and required guest services are implemented.");
    } else if (game_card_path != NULL) {
        SDL_Log("Android Game Card mounted; waiting for the user-supplied "
                "ARM7 BIOS selection.");
    } else {
        SDL_Log("Android file picker opened. Select the user-owned .nds image, "
                "then the user-owned 16 KiB ARM7 BIOS.");
    }
#endif
#else
    if (game_card_program_loaded) {
        SDL_Log(
            "Oracis host started: user Game Card ARM9/ARM7 segments loaded; "
            "native execution remains disabled.");
    } else {
        SDL_Log(
            "Oracis host started: NDS machine initialized; "
            "two %d x %d prototype screens (no static game banks linked).",
            ORACIS_SCREEN_WIDTH,
            ORACIS_SCREEN_HEIGHT
        );
    }
#endif

    exit_code = EXIT_SUCCESS;

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_ESCAPE && !event.key.repeat) {
                    running = false;
                } else if (!event.key.repeat) {
                    update_keypad_key(event.key.key, true);
                }
            } else if (event.type == SDL_EVENT_KEY_UP) {
                update_keypad_key(event.key.key, false);
            } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                release_all_keys();
            }
        }

        if (!running) {
            break;
        }

#ifdef __ANDROID__
        {
            int selected_kind = 0;
            int selected_result = 0;
            char *selected_path = NULL;
            if (take_android_file_dialog_result(
                    &selected_kind, &selected_result, &selected_path)) {
                if (selected_result == ORACIS_ANDROID_DIALOG_SELECTED &&
                    selected_path != NULL) {
                    if (selected_kind == ORACIS_ANDROID_FILE_GAME_CARD) {
                        SDL_free(g_android_game_card_uri);
                        g_android_game_card_uri = selected_path;
                        game_card_path = g_android_game_card_uri;

                        if (mount_game_card_file(game_card_path)) {
                            if (arm7_bios_path != NULL) {
                                if (load_arm7_bios_file(arm7_bios_path)) {
                                    const int load_status =
                                        oracis_host_runtime_load_mounted_game_card_program();
                                    if (load_status == ORACIS_HOST_RUNTIME_OK) {
                                        game_card_program_loaded = true;
                                        log_android_entrypoint_preflight();
                                    } else {
                                        SDL_LogCritical(
                                            SDL_LOG_CATEGORY_APPLICATION,
                                            "Android program segment loading failed "
                                            "(bridge=%d, machine=%d).",
                                            load_status,
                                            oracis_host_runtime_last_machine_status());
                                    }
                                }
                            } else if (!begin_android_file_dialog(
                                           window, ORACIS_ANDROID_FILE_ARM7_BIOS)) {
                                SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                                    "Could not start the Android ARM7 BIOS picker.");
                            }
                        }
                    } else if (selected_kind == ORACIS_ANDROID_FILE_ARM7_BIOS) {
                        SDL_free(g_android_arm7_bios_uri);
                        g_android_arm7_bios_uri = selected_path;
                        arm7_bios_path = g_android_arm7_bios_uri;

                        if (load_arm7_bios_file(arm7_bios_path)) {
                            if (!begin_android_file_dialog(
                                    window, ORACIS_ANDROID_FILE_FIRMWARE)) {
                                SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                                    "Could not start the Android SPI firmware picker.");
                            }
                        }
                    } else if (selected_kind == ORACIS_ANDROID_FILE_FIRMWARE) {
                        SDL_free(g_android_firmware_uri);
                        g_android_firmware_uri = selected_path;
                        firmware_path = g_android_firmware_uri;

                        if (load_firmware_file(firmware_path)) {
                            firmware_loaded = true;
                            const int load_status =
                                oracis_host_runtime_load_mounted_game_card_program();
                            if (load_status == ORACIS_HOST_RUNTIME_OK) {
                                game_card_program_loaded = true;
                                SDL_Log("Validated and loaded ARM9/ARM7 program "
                                        "segments from the selected NDS image.");
                                log_android_entrypoint_preflight();
                            } else {
                                SDL_LogCritical(
                                    SDL_LOG_CATEGORY_APPLICATION,
                                    "Android program segment loading failed "
                                    "(bridge=%d, machine=%d).",
                                    load_status,
                                    oracis_host_runtime_last_machine_status());
                            }
                        }
                    } else {
                        SDL_free(selected_path);
                    }
                } else {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Android file selection was cancelled or failed "
                        "(kind=%d, result=%d). Relaunch Oracis to choose again.",
                        selected_kind, selected_result);
                    SDL_free(selected_path);
                }
            }
        }
#endif

#ifdef __ANDROID__
        if (g_android_guest_execution_enabled && game_card_program_loaded) {
            /*
             * The generated-bank ABI dispatches linked guest blocks through
             * nested host calls. Keep each scheduler slice short so loops
             * unwind before the Android thread's native stack is exhausted.
             * Aggregate work per presentation iteration remains 128 * 1024
             * bus cycles, matching the previous 131072-cycle budget.
             */
            for (uint32_t round = 0u; round < 128u; ++round) {
                const int round_status = oracis_host_runtime_run_round(
                    UINT64_C(1024), NULL, NULL);
                if (round_status != ORACIS_HOST_RUNTIME_OK) {
                    g_android_guest_execution_enabled = false;
                    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                        "Native direct-entry stopped at bus cycle %llu "
                        "(round=%u, bridge=%d, machine=%d).",
                        (unsigned long long)oracis_host_runtime_global_bus_cycles(),
                        round,
                        round_status,
                        oracis_host_runtime_last_machine_status());
                    break;
                }
            }

            if (g_android_guest_execution_enabled) {
                const uint64_t cycles = oracis_host_runtime_global_bus_cycles();
                if (cycles - g_android_last_cycle_report >= UINT64_C(33554432)) {
                    SDL_Log("Native direct-entry is progressing: %llu bus cycles.",
                        (unsigned long long)cycles);
                    g_android_last_cycle_report = cycles;
                }
            }
        }
#endif

        if (draw_composed_screens(
                renderer, screen_textures, screen_pixels)) {
            SDL_RenderPresent(renderer);
        }
    }

cleanup:
    release_all_keys();
#ifdef __ANDROID__
    if (g_android_dialog_mutex != NULL) {
        SDL_LockMutex(g_android_dialog_mutex);
        SDL_free(g_android_dialog_path);
        g_android_dialog_path = NULL;
        SDL_UnlockMutex(g_android_dialog_mutex);
        SDL_DestroyMutex(g_android_dialog_mutex);
        g_android_dialog_mutex = NULL;
    }
    SDL_free(g_android_game_card_uri);
    SDL_free(g_android_arm7_bios_uri);
    SDL_free(g_android_firmware_uri);
    g_android_game_card_uri = NULL;
    g_android_arm7_bios_uri = NULL;
    g_android_firmware_uri = NULL;
#endif
    for (uint32_t engine = 0u; engine < 2u; ++engine) {
        if (screen_textures[engine] != NULL) {
            SDL_DestroyTexture(screen_textures[engine]);
        }
        free(screen_pixels[engine]);
    }
    if (host_runtime_initialized) {
        const int shutdown_status = oracis_host_runtime_shutdown();
        if (shutdown_status != ORACIS_HOST_RUNTIME_OK) {
            SDL_LogCritical(
                SDL_LOG_CATEGORY_APPLICATION,
                "NDS machine shutdown failed (bridge=%d, machine=%d).",
                shutdown_status,
                oracis_host_runtime_last_machine_status()
            );
            exit_code = EXIT_FAILURE;
        }
    }
    if (renderer != NULL) {
        SDL_DestroyRenderer(renderer);
    }
    if (window != NULL) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();

    return exit_code;
}
