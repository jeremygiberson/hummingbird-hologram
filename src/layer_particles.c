/*
 * layer_particles.c — Particles Dance rendering layer
 *
 * Audio-reactive particle effect. Renders 600 glowing dots arranged in
 * circular orbits via a fullscreen shader, with positions and brightness
 * driven by audio frequency bands.
 *
 * The expensive 600-iteration shader runs at a fixed internal resolution
 * (400x400) regardless of screen size, then is blitted up to the scene FBO.
 * This keeps GPU cost constant and manageable on the Pi 5.
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
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

/* Internal render resolution for the particle shader.
 * The 600-iteration loop runs at this resolution, then is upscaled.
 * 400x400 = 160K pixels × 600 = 96M ops (vs 384M at 800x800). */
#define PARTICLE_FBO_W  400
#define PARTICLE_FBO_H  400

/* ------------------------------------------------------------------ */
/* Private data                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    GLuint prog;
    GLuint blit_prog;

    /* Internal FBO */
    GLuint fbo;
    GLuint fbo_tex;

    /* Cached uniform locations */
    GLint u_resolution;
    GLint u_time;
    GLint u_bass;
    GLint u_mid;
    GLint u_high;
    GLint u_energy;

    int scene_width, scene_height;
    float time_accum;
} ParticlesData;

/* ------------------------------------------------------------------ */
/* Layer vtable implementation                                         */
/* ------------------------------------------------------------------ */

static bool particles_init(Layer *self, int fb_width, int fb_height) {
    ParticlesData *d = (ParticlesData *)self->user_data;
    d->scene_width  = fb_width;
    d->scene_height = fb_height;
    d->time_accum = 0.0f;

    /* Load particle shader */
    d->prog = shader_load(
        SHADER_PATH("fullscreen.vert"), SHADER_PATH("particles.frag"));
    if (!d->prog) {
        fprintf(stderr, "[particles] Failed to load particle shader\n");
        return false;
    }

    /* Cache uniform locations */
    d->u_resolution = glGetUniformLocation(d->prog, "u_resolution");
    d->u_time       = glGetUniformLocation(d->prog, "u_time");
    d->u_bass       = glGetUniformLocation(d->prog, "u_bass");
    d->u_mid        = glGetUniformLocation(d->prog, "u_mid");
    d->u_high       = glGetUniformLocation(d->prog, "u_high");
    d->u_energy     = glGetUniformLocation(d->prog, "u_energy");

    /* Load blit shader */
    d->blit_prog = shader_load(
        SHADER_PATH("fullscreen.vert"), SHADER_PATH("blit.frag"));
    if (!d->blit_prog) {
        fprintf(stderr, "[particles] Failed to load blit shader\n");
        return false;
    }

    /* Create internal FBO at fixed resolution */
    if (!create_fbo_color(&d->fbo, &d->fbo_tex, PARTICLE_FBO_W, PARTICLE_FBO_H)) {
        fprintf(stderr, "[particles] Failed to create internal FBO\n");
        return false;
    }

    fprintf(stderr, "[particles] Initialized: scene %dx%d, internal %dx%d\n",
            d->scene_width, d->scene_height, PARTICLE_FBO_W, PARTICLE_FBO_H);
    return true;
}

static void particles_update(Layer *self, const AudioBands *bands, float dt) {
    (void)bands;
    ParticlesData *d = (ParticlesData *)self->user_data;
    d->time_accum += dt;
}

static void particles_draw(Layer *self, const AudioBands *bands) {
    ParticlesData *d = (ParticlesData *)self->user_data;
    GLuint scene_fbo = renderer_get_scene_fbo();
    int scene_w, scene_h;
    renderer_get_dimensions(&scene_w, &scene_h);

    /* --- Pass 1: Render particles into internal FBO --- */
    glBindFramebuffer(GL_FRAMEBUFFER, d->fbo);
    glViewport(0, 0, PARTICLE_FBO_W, PARTICLE_FBO_H);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glUseProgram(d->prog);

    if (d->u_resolution >= 0)
        glUniform2f(d->u_resolution, (float)PARTICLE_FBO_W, (float)PARTICLE_FBO_H);
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

    /* --- Pass 2: Blit internal FBO to scene FBO with additive blending --- */
    glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
    glViewport(0, 0, scene_w, scene_h);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);

    glUseProgram(d->blit_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d->fbo_tex);
    glUniform1i(glGetUniformLocation(d->blit_prog, "u_texture"), 0);

    draw_fullscreen_quad();

    /* Restore GL state */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
}

static void particles_resize(Layer *self, int fb_width, int fb_height) {
    ParticlesData *d = (ParticlesData *)self->user_data;
    d->scene_width  = fb_width;
    d->scene_height = fb_height;
    fprintf(stderr, "[particles] Resize: scene %dx%d\n", fb_width, fb_height);
}

static void particles_shutdown(Layer *self) {
    ParticlesData *d = (ParticlesData *)self->user_data;
    if (!d) return;

    if (d->prog)     glDeleteProgram(d->prog);
    if (d->blit_prog) glDeleteProgram(d->blit_prog);
    if (d->fbo)      glDeleteFramebuffers(1, &d->fbo);
    if (d->fbo_tex)  glDeleteTextures(1, &d->fbo_tex);

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
