#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "oracis/memory/guest_memory_abi.h"
#include "oracis/host/runtime_bridge.h"

#include <stdbool.h>
#include <stdint.h>
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

static bool mount_game_card_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "Could not open Game Card image '%s'.", path);
        return false;
    }

    if (fseek(file, 0L, SEEK_END) != 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "Could not determine Game Card image size for '%s'.", path);
        (void)fclose(file);
        return false;
    }

    const long file_size = ftell(file);
    if (file_size < 0x200L ||
        (unsigned long)file_size > 0x20000000UL) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "Game Card image '%s' has an invalid size (%ld bytes); "
            "expected 512 bytes through 512 MiB.",
            path, file_size);
        (void)fclose(file);
        return false;
    }

    if (fseek(file, 0L, SEEK_SET) != 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "Could not rewind Game Card image '%s'.", path);
        (void)fclose(file);
        return false;
    }

    const size_t image_size = (size_t)file_size;
    void *image = malloc(image_size);
    if (image == NULL) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "Could not allocate %zu bytes for Game Card image '%s'.",
            image_size, path);
        (void)fclose(file);
        return false;
    }

    const size_t bytes_read = fread(image, 1u, image_size, file);
    const bool read_ok = bytes_read == image_size && ferror(file) == 0;
    const int close_status = fclose(file);
    if (!read_ok || close_status != 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
            "Could not read the complete Game Card image '%s'.", path);
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

    SDL_Log("Mounted local Game Card image '%s' (%zu bytes). "
            "Only unencrypted B7 reads are currently supported.",
            path, image_size);
    return true;
}

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
                    "OBJ=%u, OBJ index=%u). The frame was not presented.",
                    engine_index,
                    scanline,
                    status,
                    report.status,
                    report.background_index,
                    report.fetch_status,
                    report.memory_status,
                    report.object_fetch_status,
                    report.object_index);
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
                    "Usage: %s [--game-card path/to/game.nds]", argv[0]);
                goto cleanup;
            }
            game_card_path = argv[++argument];
        } else if (strcmp(argv[argument], "--help") == 0 ||
                   strcmp(argv[argument], "-h") == 0) {
            SDL_Log("Usage: %s [--game-card path/to/game.nds]", argv[0]);
            exit_code = EXIT_SUCCESS;
            goto cleanup;
        } else {
            SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                "Unknown argument '%s'. Use --help for usage.", argv[argument]);
            goto cleanup;
        }
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

    if (game_card_path != NULL && !mount_game_card_file(game_card_path)) {
        goto cleanup;
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
    const int image_status =
        oracis_host_runtime_load_local_platinum_segments(NULL);
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
        SDL_Log(
            "Local Platinum USA Rev 1 ARM9/ARM7 images were size/hash verified "
            "and loaded into guest memory. Native execution remains disabled "
            "until the IRQ provider and required runtime services are complete."
        );
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
    SDL_Log(
        "Local Platinum ARM9/ARM7 banks registered. Android file selection "
        "and local program-image loading are not wired yet."
    );
#endif
#else
    SDL_Log(
        "Oracis host started: NDS machine initialized; "
        "two %d x %d prototype screens (no static game banks linked).",
        ORACIS_SCREEN_WIDTH,
        ORACIS_SCREEN_HEIGHT
    );
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

        if (draw_composed_screens(
                renderer, screen_textures, screen_pixels)) {
            SDL_RenderPresent(renderer);
        }
    }

cleanup:
    release_all_keys();
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
