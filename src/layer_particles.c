/*
 * layer_particles.c — Particles Dance rendering layer
 *
 * Audio-reactive particle effect. Renders 600 glowing dots arranged in
 * circular orbits via a fullscreen shader, with positions and brightness
 * driven by audio frequency bands.
 *
 * Adapted from Shadertoy "Particles Dance"
 * (https://www.shadertoy.com/view/MdfBz7)
 */
#include "layer_particles.h"
#include "renderer.h"
#include "shader.h"

#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Private data                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    GLuint prog;

    /* Cached uniform locations */
    GLint u_resolution;
    GLint u_time;
    GLint u_bass;
    GLint u_mid;
    GLint u_high;
    GLint u_energy;

    int width, height;
    float time_accum;
} ParticlesData;

/* ------------------------------------------------------------------ */
/* Layer vtable implementation                                         */
/* ------------------------------------------------------------------ */

static bool particles_init(Layer *self, int fb_width, int fb_height) {
    ParticlesData *d = (ParticlesData *)self->user_data;
    d->width  = fb_width;
    d->height = fb_height;
    d->time_accum = 0.0f;

    d->prog = shader_load(
        SHADER_PATH("fullscreen.vert"), SHADER_PATH("particles.frag"));
    if (!d->prog) {
        fprintf(stderr, "[particles] Failed to load shader\n");
        return false;
    }

    /* Cache uniform locations (attribs already bound by default to 0,1) */
    d->u_resolution = glGetUniformLocation(d->prog, "u_resolution");
    d->u_time       = glGetUniformLocation(d->prog, "u_time");
    d->u_bass       = glGetUniformLocation(d->prog, "u_bass");
    d->u_mid        = glGetUniformLocation(d->prog, "u_mid");
    d->u_high       = glGetUniformLocation(d->prog, "u_high");
    d->u_energy     = glGetUniformLocation(d->prog, "u_energy");

    fprintf(stderr, "[particles] Initialized: %dx%d\n", d->width, d->height);
    return true;
}

static void particles_update(Layer *self, const AudioBands *bands, float dt) {
    (void)bands;
    ParticlesData *d = (ParticlesData *)self->user_data;
    d->time_accum += dt;
}

static void particles_draw(Layer *self, const AudioBands *bands) {
    ParticlesData *d = (ParticlesData *)self->user_data;

    /* Disable depth test — this is a fullscreen 2D effect */
    glDisable(GL_DEPTH_TEST);

    /* Use additive blending so particles glow on top of black / other layers */
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);

    glUseProgram(d->prog);

    if (d->u_resolution >= 0)
        glUniform2f(d->u_resolution, (float)d->width, (float)d->height);
    if (d->u_time >= 0)
        glUniform1f(d->u_time, d->time_accum);
    if (d->u_bass >= 0)
        glUniform1f(d->u_bass, bands->bass);
    if (d->u_mid >= 0)
        glUniform1f(d->u_mid, bands->mid);
    if (d->u_high >= 0)
        glUniform1f(d->u_high, bands->high);
    if (d->u_energy >= 0)
        glUniform1f(d->u_energy, bands->energy);

    draw_fullscreen_quad();

    /* Restore GL state */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
}

static void particles_resize(Layer *self, int fb_width, int fb_height) {
    ParticlesData *d = (ParticlesData *)self->user_data;
    d->width  = fb_width;
    d->height = fb_height;
    fprintf(stderr, "[particles] Resize: %dx%d\n", fb_width, fb_height);
}

static void particles_shutdown(Layer *self) {
    ParticlesData *d = (ParticlesData *)self->user_data;
    if (!d) return;

    if (d->prog) glDeleteProgram(d->prog);

    free(d);
    self->user_data = NULL;

    fprintf(stderr, "[particles] Shutdown\n");
}

/* ------------------------------------------------------------------ */
/* Public constructor                                                  */
/* ------------------------------------------------------------------ */

Layer *layer_particles_create(void) {
    Layer *layer = calloc(1, sizeof(Layer));
    if (!layer) return NULL;

    ParticlesData *d = calloc(1, sizeof(ParticlesData));
    if (!d) {
        free(layer);
        return NULL;
    }

    layer->name          = "Particles";
    layer->option_count  = 1;
    layer->current_option = 0;  /* off by default */

    layer->scale[0] = 1.0f;
    layer->scale[1] = 1.0f;
    layer->scale[2] = 1.0f;

    layer->init     = particles_init;
    layer->update   = particles_update;
    layer->draw     = particles_draw;
    layer->resize   = particles_resize;
    layer->shutdown = particles_shutdown;

    layer->user_data = d;

    return layer;
}
