#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "oracis/memory/guest_memory_abi.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

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

static void draw_test_screens(SDL_Renderer *renderer)
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

    SDL_SetRenderDrawColor(renderer, 18, 20, 28, 255);
    SDL_RenderClear(renderer);

    /* Temporary stand-in colors: the real game renderer is a later phase. */
    SDL_SetRenderDrawColor(renderer, 42, 58, 78, 255);
    SDL_RenderFillRect(renderer, &top_screen);

    SDL_SetRenderDrawColor(renderer, 70, 76, 88, 255);
    SDL_RenderFillRect(renderer, &bottom_screen);
}

int main(int argc, char *argv[])
{
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Event event;
    bool running = true;
    int exit_code = EXIT_FAILURE;

    (void)argc;
    (void)argv;

    oracis_host_set_keypad_state(0u);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_LogCritical(
            SDL_LOG_CATEGORY_APPLICATION,
            "SDL initialization failed: %s",
            SDL_GetError()
        );
        return EXIT_FAILURE;
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

    SDL_Log("Oracis host started: two %d x %d test screens.",
            ORACIS_SCREEN_WIDTH,
            ORACIS_SCREEN_HEIGHT);

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

        draw_test_screens(renderer);
        SDL_RenderPresent(renderer);
    }

cleanup:
    release_all_keys();
    if (renderer != NULL) {
        SDL_DestroyRenderer(renderer);
    }
    if (window != NULL) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();

    return exit_code;
}
