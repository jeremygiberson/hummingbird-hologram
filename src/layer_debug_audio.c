/*
 * layer_debug_audio.c — Debug overlay for audio analysis values
 *
 * Renders horizontal bars in the top-left corner of the screen.
 * Each bar represents one AudioBands field, colored distinctly:
 *
 *   Red     = bass
 *   Yellow  = mid
 *   Cyan    = high
 *   White   = energy
 *   Magenta = beat
 *   Orange  = onset
 *   Green   = spectral centroid
 *   Blue    = spectral flux
 *
 * Bars are drawn as quads directly in NDC. A thin dark background
 * strip is drawn behind each bar for contrast.
 */
#include "layer_debug_audio.h"
#include "renderer.h"
#include "shader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define NUM_BARS       9
#define BAR_HEIGHT_NDC 0.025f   /* Height of each bar in NDC (-1..1) */
#define BAR_GAP_NDC    0.008f   /* Gap between bars */
#define BAR_MAX_W_NDC  0.5f     /* Max bar width (value=1.0) */
#define BAR_X_START   -0.98f    /* Left edge of bars */
#define BAR_Y_START    0.98f    /* Top edge (first bar) */

/* 6 vertices per bar (2 triangles), 2 bars per value (background + fill) */
#define VERTS_PER_QUAD 6
#define FLOATS_PER_VERT 5       /* x, y, r, g, b */
#define MAX_VERTS (NUM_BARS * 2 * VERTS_PER_QUAD)
#define VBO_SIZE  (MAX_VERTS * FLOATS_PER_VERT * sizeof(float))

/* ------------------------------------------------------------------ */
/* Bar definitions                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *label;
    float r, g, b;
} BarDef;

static const BarDef BAR_DEFS[NUM_BARS] = {
    { "bass",     1.0f, 0.2f, 0.2f },  /* Red */
    { "mid",      1.0f, 1.0f, 0.2f },  /* Yellow */
    { "high",     0.2f, 1.0f, 1.0f },  /* Cyan */
    { "energy",   1.0f, 1.0f, 1.0f },  /* White */
    { "beat",     1.0f, 0.2f, 1.0f },  /* Magenta */
    { "onset",    1.0f, 0.6f, 0.1f },  /* Orange */
    { "centroid", 0.2f, 1.0f, 0.2f },  /* Green */
    { "flux",     0.3f, 0.4f, 1.0f },  /* Blue */
    { "fps",      0.8f, 0.8f, 0.8f },  /* Gray */
};

/* ------------------------------------------------------------------ */
/* Private data                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    GLuint prog;
    GLuint vbo;
    int width, height;
    float verts[MAX_VERTS * FLOATS_PER_VERT];
    float fps;           /* Smoothed FPS for display */
} DebugAudioData;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Append a colored quad (2 triangles, 6 verts) into the vertex buffer.
 * Returns the number of floats written (30). */
static int push_quad(float *buf, float x0, float y0, float x1, float y1,
                     float r, float g, float b) {
    /* Triangle 1: top-left, bottom-left, bottom-right */
    float verts[] = {
        x0, y1, r, g, b,
        x0, y0, r, g, b,
        x1, y0, r, g, b,
        /* Triangle 2: top-left, bottom-right, top-right */
        x0, y1, r, g, b,
        x1, y0, r, g, b,
        x1, y1, r, g, b,
    };
    memcpy(buf, verts, sizeof(verts));
    return VERTS_PER_QUAD * FLOATS_PER_VERT;
}

static float get_band_value(const AudioBands *bands, int index,
                            const DebugAudioData *d) {
    switch (index) {
    case 0: return bands->bass;
    case 1: return bands->mid;
    case 2: return bands->high;
    case 3: return bands->energy;
    case 4: return bands->beat;
    case 5: return bands->onset;
    case 6: return bands->spectral_centroid;
    case 7: return bands->spectral_flux;
    case 8: return d->fps / 60.0f;  /* Normalized: 60fps = full bar */
    default: return 0.0f;
    }
}

/* ------------------------------------------------------------------ */
/* Layer vtable                                                        */
/* ------------------------------------------------------------------ */

static bool debug_audio_init(Layer *self, int fb_width, int fb_height) {
    DebugAudioData *d = (DebugAudioData *)self->user_data;
    d->width  = fb_width;
    d->height = fb_height;

    d->prog = shader_load(
        SHADER_PATH("debug_bars.vert"), SHADER_PATH("debug_bars.frag"));
    if (!d->prog) {
        fprintf(stderr, "[debug_audio] Failed to load shader\n");
        return false;
    }

    glBindAttribLocation(d->prog, 0, "a_position");
    glBindAttribLocation(d->prog, 1, "a_color");
    glLinkProgram(d->prog);

    glGenBuffers(1, &d->vbo);

    fprintf(stderr, "[debug_audio] Initialized: %dx%d\n", d->width, d->height);
    return true;
}

static void debug_audio_update(Layer *self, const AudioBands *bands, float dt) {
    (void)bands;
    DebugAudioData *d = (DebugAudioData *)self->user_data;
    float instant_fps = (dt > 0.0001f) ? 1.0f / dt : 0.0f;
    /* Smooth FPS with EMA to avoid jitter */
    d->fps = d->fps * 0.9f + instant_fps * 0.1f;
}

static void debug_audio_draw(Layer *self, const AudioBands *bands) {
    DebugAudioData *d = (DebugAudioData *)self->user_data;
    int offset = 0;

    for (int i = 0; i < NUM_BARS; i++) {
        float y_top = BAR_Y_START - (float)i * (BAR_HEIGHT_NDC + BAR_GAP_NDC);
        float y_bot = y_top - BAR_HEIGHT_NDC;

        /* Background bar (dark, full width) */
        offset += push_quad(d->verts + offset,
                            BAR_X_START, y_bot,
                            BAR_X_START + BAR_MAX_W_NDC, y_top,
                            0.15f, 0.15f, 0.15f);

        /* Foreground bar (colored, width = value) */
        float val = get_band_value(bands, i, d);
        if (val < 0.0f) val = 0.0f;
        if (val > 1.0f) val = 1.0f;
        float w = val * BAR_MAX_W_NDC;
        if (w < 0.002f) w = 0.002f;  /* Always show a sliver */

        const BarDef *def = &BAR_DEFS[i];
        offset += push_quad(d->verts + offset,
                            BAR_X_START, y_bot,
                            BAR_X_START + w, y_top,
                            def->r, def->g, def->b);
    }

    int vert_count = offset / FLOATS_PER_VERT;

    /* Upload and draw */
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(d->prog);

    glBindBuffer(GL_ARRAY_BUFFER, d->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(offset * sizeof(float)),
                 d->verts, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                          FLOATS_PER_VERT * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
                          FLOATS_PER_VERT * sizeof(float),
                          (void *)(2 * sizeof(float)));

    glDrawArrays(GL_TRIANGLES, 0, vert_count);

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);

    /* Restore state */
    glEnable(GL_DEPTH_TEST);

    /* Rebind scene FBO (draw may have been called while scene FBO is bound) */
    glBindFramebuffer(GL_FRAMEBUFFER, renderer_get_scene_fbo());
}

static void debug_audio_resize(Layer *self, int fb_width, int fb_height) {
    DebugAudioData *d = (DebugAudioData *)self->user_data;
    d->width  = fb_width;
    d->height = fb_height;
}

static void debug_audio_shutdown(Layer *self) {
    DebugAudioData *d = (DebugAudioData *)self->user_data;
    if (!d) return;

    if (d->prog) glDeleteProgram(d->prog);
    if (d->vbo)  glDeleteBuffers(1, &d->vbo);

    free(d);
    self->user_data = NULL;

    fprintf(stderr, "[debug_audio] Shutdown\n");
}

/* ------------------------------------------------------------------ */
/* Public constructor                                                  */
/* ------------------------------------------------------------------ */

Layer *layer_debug_audio_create(void) {
    Layer *layer = calloc(1, sizeof(Layer));
    if (!layer) return NULL;

    DebugAudioData *d = calloc(1, sizeof(DebugAudioData));
    if (!d) {
        free(layer);
        return NULL;
    }

    layer->name          = "Debug Audio";
    layer->option_count  = 1;
    layer->current_option = 0;  /* off by default */

    layer->scale[0] = 1.0f;
    layer->scale[1] = 1.0f;
    layer->scale[2] = 1.0f;

    layer->init     = debug_audio_init;
    layer->update   = debug_audio_update;
    layer->draw     = debug_audio_draw;
    layer->resize   = debug_audio_resize;
    layer->shutdown = debug_audio_shutdown;

    layer->user_data = d;

    return layer;
}
