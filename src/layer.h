/*
 * layer.h — Composable rendering layer interface
 *
 * Each layer is a self-contained rendering unit with its own shaders,
 * state, and animation. Layers draw into the shared scene FBO managed
 * by renderer.c.
 */
#pragma once

#include "platform.h"
#include "audio.h"
#include <stdbool.h>

typedef struct Layer {
    const char *name;
    int option_count;       /* number of active modes (1 = simple on/off) */
    int current_option;     /* 0 = off, 1..option_count = active mode */

    /* Per-layer transform (written by update, read by draw) */
    float position[3];
    float rotation[3];      /* euler degrees, applied X->Y->Z */
    float scale[3];         /* default {1,1,1} */

    /* Vtable */
    bool (*init)(struct Layer *self, int fb_width, int fb_height);
    void (*update)(struct Layer *self, const AudioBands *bands, float dt);
    void (*draw)(struct Layer *self, const AudioBands *bands);
    void (*resize)(struct Layer *self, int fb_width, int fb_height);
    void (*shutdown)(struct Layer *self);

    void *user_data;
} Layer;
