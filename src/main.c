/*
 * main.c — Hologram Hummingbird Music Visualizer
 *
 * Entry point: SDL2 initialization, GL context, main loop.
 * Designed to run identically on macOS (dev) and Pi Zero 2W (target).
 */
#include "platform.h"
#include "renderer.h"
#include "audio.h"
#include "model.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define TARGET_FPS       30
#define FRAME_TIME_MS    (1000 / TARGET_FPS)

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int win_w = HOLOGRAM_DEFAULT_WIDTH;
    int win_h = HOLOGRAM_DEFAULT_HEIGHT;

    /* Parse optional --width / --height args */
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--width") == 0)  win_w = atoi(argv[i + 1]);
        if (strcmp(argv[i], "--height") == 0) win_h = atoi(argv[i + 1]);
    }

    /* --- SDL init --- */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[main] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    platform_set_gl_attributes();

    Uint32 win_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
#ifdef PLATFORM_PI
    /* On Pi with KMSDRM, use fullscreen */
    win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif

    SDL_Window *window = SDL_CreateWindow(
        "Hologram Hummingbird",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        win_flags
    );
    if (!window) {
        fprintf(stderr, "[main] Window creation failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        fprintf(stderr, "[main] GL context creation failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* VSync: 1 = enabled, 0 = disabled */
    SDL_GL_SetSwapInterval(1);

    /* Get actual framebuffer size (may differ from window on HiDPI) */
    int fb_w, fb_h;
    SDL_GL_GetDrawableSize(window, &fb_w, &fb_h);
    fprintf(stderr, "[main] Window: %dx%d, Framebuffer: %dx%d\n",
            win_w, win_h, fb_w, fb_h);

    /* Print GL info */
    fprintf(stderr, "[main] GL Vendor:   %s\n", glGetString(GL_VENDOR));
    fprintf(stderr, "[main] GL Renderer: %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "[main] GL Version:  %s\n", glGetString(GL_VERSION));
    fprintf(stderr, "[main] GLSL:        %s\n",
            glGetString(GL_SHADING_LANGUAGE_VERSION));

    /* --- Initialize subsystems --- */
    if (!renderer_init(fb_w, fb_h)) {
        fprintf(stderr, "[main] Renderer init failed\n");
        goto cleanup;
    }

    if (!audio_init()) {
        fprintf(stderr, "[main] Audio init failed\n");
        goto cleanup;
    }

    /* Load model */
    Model *model = model_load(ASSET_PATH("hummingbird.glb"));
    if (!model) {
        fprintf(stderr, "[main] Model not found — using debug icosahedron\n");
        model = model_create_debug();
    }
    if (!model) {
        fprintf(stderr, "[main] Failed to create any model\n");
        goto cleanup;
    }

    /* --- Main loop --- */
    fprintf(stderr, "[main] Entering main loop at %d FPS\n", TARGET_FPS);

    bool running = true;
    Uint32 last_time = SDL_GetTicks();

    while (running) {
        Uint32 frame_start = SDL_GetTicks();

        /* --- Events --- */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_ESCAPE ||
                    ev.key.keysym.sym == SDLK_q) {
                    running = false;
                }
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    SDL_GL_GetDrawableSize(window, &fb_w, &fb_h);
                    renderer_resize(fb_w, fb_h);
                }
                break;
            }
        }

        /* --- Timing --- */
        Uint32 now = SDL_GetTicks();
        float dt = (float)(now - last_time) / 1000.0f;
        last_time = now;

        /* Clamp dt to avoid spiral of death */
        if (dt > 0.1f) dt = 0.1f;

        /* --- Update --- */
        audio_update();
        AudioBands bands = audio_get_bands();

        /* --- Render --- */
        renderer_frame(model, &bands, dt);
        SDL_GL_SwapWindow(window);

        /* --- Frame limiting --- */
        Uint32 frame_elapsed = SDL_GetTicks() - frame_start;
        if (frame_elapsed < FRAME_TIME_MS) {
            SDL_Delay(FRAME_TIME_MS - frame_elapsed);
        }
    }

    /* --- Cleanup --- */
cleanup:
    fprintf(stderr, "[main] Shutting down\n");
    if (model) model_destroy(model);
    audio_shutdown();
    renderer_shutdown();
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
