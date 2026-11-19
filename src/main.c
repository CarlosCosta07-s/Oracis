#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdbool.h>
#include <stdlib.h>

enum {
    ORACIS_SCREEN_WIDTH = 256,
    ORACIS_SCREEN_HEIGHT = 192,
    ORACIS_SCREEN_GAP = 8,
    ORACIS_LOGICAL_HEIGHT = (ORACIS_SCREEN_HEIGHT * 2) + ORACIS_SCREEN_GAP
};

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
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
                event.type == SDL_EVENT_TERMINATING) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN &&
                       event.key.key == SDLK_ESCAPE &&
                       !event.key.repeat) {
                running = false;
            }
        }

        if (!running) {
            break;
        }

        draw_test_screens(renderer);
        SDL_RenderPresent(renderer);
    }

cleanup:
    if (renderer != NULL) {
        SDL_DestroyRenderer(renderer);
    }
    if (window != NULL) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();

    return exit_code;
}
