/*
 * layer_hummingbird.c — Hummingbird model rendering layer
 *
 * Extracts all model-specific rendering from the old renderer.c.
 * Supports two modes:
 *   Option 1: Render hummingbird without bloom
 *   Option 2: Render hummingbird with bloom
 */
#include "layer_hummingbird.h"
#include "renderer.h"
#include "model.h"
#include "shader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Bloom configuration                                                 */
/* ------------------------------------------------------------------ */

#define BLOOM_BLUR_PASSES   3       /* Number of blur ping-pong iterations */
#define BLOOM_DOWNSAMPLE    2       /* Bloom FBOs at 1/N resolution */
#define BLOOM_THRESHOLD     0.85f   /* Brightness cutoff for extraction */
#define MOTION_BLUR_SAMPLES 1       /* 1 = disabled for now */

/* ------------------------------------------------------------------ */
/* Private data                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    Model *model;

    /* Scene shader */
    GLuint scene_prog;
    ModelUniforms model_uniforms;

    /* Bloom shaders */
    GLuint extract_prog;
    GLuint blur_prog;

    /* Bloom FBOs (ping-pong) */
    GLuint fbo_bloom[2];
    GLuint tex_bloom[2];

    /* Pre-bloom FBO (color + depth, for with-bloom path) */
    GLuint fbo_pre_bloom;
    GLuint tex_pre_bloom;
    GLuint rbo_pre_bloom_depth;

    /* Dimensions */
    int width, height;
    int bloom_w, bloom_h;

    /* Stashed dt from update for use in draw (needed for motion blur) */
    float stashed_dt;
} HummingbirdData;

/* ------------------------------------------------------------------ */
/* FBO helpers                                                         */
/* ------------------------------------------------------------------ */

static bool create_pre_bloom_fbo(HummingbirdData *d) {
    glGenFramebuffers(1, &d->fbo_pre_bloom);
    glGenTextures(1, &d->tex_pre_bloom);
    glGenRenderbuffers(1, &d->rbo_pre_bloom_depth);

    /* Color attachment */
    glBindTexture(GL_TEXTURE_2D, d->tex_pre_bloom);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, d->width, d->height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* Depth attachment */
    glBindRenderbuffer(GL_RENDERBUFFER, d->rbo_pre_bloom_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
                          d->width, d->height);

    /* Assemble */
    glBindFramebuffer(GL_FRAMEBUFFER, d->fbo_pre_bloom);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, d->tex_pre_bloom, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_RENDERBUFFER, d->rbo_pre_bloom_depth);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[hummingbird] Pre-bloom FBO incomplete: 0x%x\n", status);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Shader loading                                                      */
/* ------------------------------------------------------------------ */

static bool load_scene_shader(HummingbirdData *d) {
    d->scene_prog = shader_load(
        SHADER_PATH("scene.vert"), SHADER_PATH("scene.frag"));
    if (!d->scene_prog) {
        fprintf(stderr, "[hummingbird] Failed to load scene shader\n");
        return false;
    }

    /* Bind attribute locations BEFORE linking (must match model.c layout) */
    glBindAttribLocation(d->scene_prog, 0, "a_position");
    glBindAttribLocation(d->scene_prog, 1, "a_normal");
    glBindAttribLocation(d->scene_prog, 2, "a_texcoord");
    glBindAttribLocation(d->scene_prog, 3, "a_joints");
    glBindAttribLocation(d->scene_prog, 4, "a_weights");
    glLinkProgram(d->scene_prog);  /* Re-link after binding attribs */

    /* Cache uniform locations */
    d->model_uniforms.u_model_matrix = glGetUniformLocation(d->scene_prog, "u_model");
    d->model_uniforms.u_view_matrix  = glGetUniformLocation(d->scene_prog, "u_view");
    d->model_uniforms.u_proj_matrix  = glGetUniformLocation(d->scene_prog, "u_proj");
    d->model_uniforms.u_texture0     = glGetUniformLocation(d->scene_prog, "u_texture0");
    d->model_uniforms.u_texture1     = glGetUniformLocation(d->scene_prog, "u_texture1");
    d->model_uniforms.u_energy       = glGetUniformLocation(d->scene_prog, "u_energy");
    d->model_uniforms.u_has_skin     = glGetUniformLocation(d->scene_prog, "u_has_skin");

    for (int i = 0; i < MAX_JOINTS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "u_joints[%d]", i);
        d->model_uniforms.u_joints[i] = glGetUniformLocation(d->scene_prog, name);
    }

    return true;
}

static bool load_bloom_shaders(HummingbirdData *d) {
    d->extract_prog = shader_load(
        SHADER_PATH("fullscreen.vert"), SHADER_PATH("bright_extract.frag"));
    d->blur_prog = shader_load(
        SHADER_PATH("fullscreen.vert"), SHADER_PATH("blur.frag"));

    if (!d->extract_prog || !d->blur_prog) {
        fprintf(stderr, "[hummingbird] Failed to load bloom shaders\n");
        return false;
    }

    /* Bind fullscreen quad attribute locations */
    GLuint progs[] = {d->extract_prog, d->blur_prog};
    for (int i = 0; i < 2; i++) {
        glBindAttribLocation(progs[i], 0, "a_position");
        glBindAttribLocation(progs[i], 1, "a_texcoord");
        glLinkProgram(progs[i]);
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* Render helper                                                       */
/* ------------------------------------------------------------------ */

static void render_model(HummingbirdData *d, const AudioBands *bands) {
    glUseProgram(d->scene_prog);

    /* Set audio-reactive uniforms */
    GLint loc;
    loc = glGetUniformLocation(d->scene_prog, "u_bass");
    if (loc >= 0) glUniform1f(loc, bands->bass);
    loc = glGetUniformLocation(d->scene_prog, "u_mid");
    if (loc >= 0) glUniform1f(loc, bands->mid);
    loc = glGetUniformLocation(d->scene_prog, "u_high");
    if (loc >= 0) glUniform1f(loc, bands->high);

    GLint u_alpha = glGetUniformLocation(d->scene_prog, "u_alpha");
    float sub_dt = d->stashed_dt / (float)MOTION_BLUR_SAMPLES;
    float alpha = 1.0f / (float)MOTION_BLUR_SAMPLES;

    /* Only use additive blending when motion blur is active (multiple sub-frames).
     * With a single sample, standard blending avoids incorrectly adding to content
     * already drawn by other layers into the shared scene FBO. */
    if (MOTION_BLUR_SAMPLES > 1) {
        glBlendFunc(GL_ONE, GL_ONE);
    }

    for (int s = 0; s < MOTION_BLUR_SAMPLES; s++) {
        if (s > 0) glClear(GL_DEPTH_BUFFER_BIT);
        if (u_alpha >= 0) glUniform1f(u_alpha, alpha);
        model_update(d->model, sub_dt);
        model_draw(d->model, &d->model_uniforms, bands->energy);
    }

    if (MOTION_BLUR_SAMPLES > 1) {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
}

/* ------------------------------------------------------------------ */
/* Draw paths                                                          */
/* ------------------------------------------------------------------ */

static void hummingbird_draw_no_bloom(HummingbirdData *d, const AudioBands *bands) {
    /* Draw directly into the shared scene FBO (already bound by renderer) */
    render_model(d, bands);
}

static void hummingbird_draw_with_bloom(HummingbirdData *d, const AudioBands *bands) {
    GLuint scene_fbo = renderer_get_scene_fbo();

    /* Step 1: Render model into private pre-bloom FBO */
    glBindFramebuffer(GL_FRAMEBUFFER, d->fbo_pre_bloom);
    glViewport(0, 0, d->width, d->height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    render_model(d, bands);

    /* Step 2: Bright extract */
    glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, d->fbo_bloom[0]);
    glViewport(0, 0, d->bloom_w, d->bloom_h);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(d->extract_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d->tex_pre_bloom);
    glUniform1i(glGetUniformLocation(d->extract_prog, "u_scene"), 0);
    glUniform1f(glGetUniformLocation(d->extract_prog, "u_threshold"), BLOOM_THRESHOLD);
    draw_fullscreen_quad();

    /* Step 3: Gaussian blur ping-pong */
    glUseProgram(d->blur_prog);
    GLint u_image = glGetUniformLocation(d->blur_prog, "u_image");
    GLint u_horizontal = glGetUniformLocation(d->blur_prog, "u_horizontal");
    GLint u_tex_size = glGetUniformLocation(d->blur_prog, "u_tex_size");

    for (int pass = 0; pass < BLOOM_BLUR_PASSES; pass++) {
        /* Horizontal: bloom[0] -> bloom[1] */
        glBindFramebuffer(GL_FRAMEBUFFER, d->fbo_bloom[1]);
        glClear(GL_COLOR_BUFFER_BIT);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, d->tex_bloom[0]);
        glUniform1i(u_image, 0);
        glUniform1i(u_horizontal, 1);
        if (u_tex_size >= 0)
            glUniform2f(u_tex_size, (float)d->bloom_w, (float)d->bloom_h);
        draw_fullscreen_quad();

        /* Vertical: bloom[1] -> bloom[0] */
        glBindFramebuffer(GL_FRAMEBUFFER, d->fbo_bloom[0]);
        glClear(GL_COLOR_BUFFER_BIT);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, d->tex_bloom[1]);
        glUniform1i(u_image, 0);
        glUniform1i(u_horizontal, 0);
        if (u_tex_size >= 0)
            glUniform2f(u_tex_size, (float)d->bloom_w, (float)d->bloom_h);
        draw_fullscreen_quad();
    }

    /* Step 4: Composite pre-bloom scene + bloom into shared scene FBO
     * Use additive blending so we add to whatever else is already in the scene */
    glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
    int scene_w, scene_h;
    renderer_get_dimensions(&scene_w, &scene_h);
    glViewport(0, 0, scene_w, scene_h);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);

    /* Draw the pre-bloom scene into scene FBO */
    glUseProgram(d->extract_prog);  /* Reuse extract as passthrough with threshold=0 */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d->tex_pre_bloom);
    glUniform1i(glGetUniformLocation(d->extract_prog, "u_scene"), 0);
    glUniform1f(glGetUniformLocation(d->extract_prog, "u_threshold"), 0.0f);
    draw_fullscreen_quad();

    /* Add bloom on top */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d->tex_bloom[0]);
    glUniform1i(glGetUniformLocation(d->extract_prog, "u_scene"), 0);
    glUniform1f(glGetUniformLocation(d->extract_prog, "u_threshold"), 0.0f);
    draw_fullscreen_quad();

    /* Restore GL state */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);

    /* Rebind shared scene FBO (already bound, but be explicit) */
    glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
}

/* ------------------------------------------------------------------ */
/* Layer vtable implementation                                         */
/* ------------------------------------------------------------------ */

static bool hummingbird_init(Layer *self, int fb_width, int fb_height) {
    HummingbirdData *d = (HummingbirdData *)self->user_data;
    d->width  = fb_width;
    d->height = fb_height;
    d->bloom_w = fb_width  / BLOOM_DOWNSAMPLE;
    d->bloom_h = fb_height / BLOOM_DOWNSAMPLE;

    /* Load model */
    d->model = model_load(ASSET_PATH("hummingbird.glb"));
    if (!d->model) {
        fprintf(stderr, "[hummingbird] Model not found -- using debug icosahedron\n");
        d->model = model_create_debug();
    }
    if (!d->model) {
        fprintf(stderr, "[hummingbird] Failed to create any model\n");
        return false;
    }

    /* Load shaders */
    if (!load_scene_shader(d)) return false;
    if (!load_bloom_shaders(d)) return false;

    /* Create bloom FBOs */
    if (!create_fbo_color(&d->fbo_bloom[0], &d->tex_bloom[0],
                          d->bloom_w, d->bloom_h))
        return false;
    if (!create_fbo_color(&d->fbo_bloom[1], &d->tex_bloom[1],
                          d->bloom_w, d->bloom_h))
        return false;

    /* Create pre-bloom FBO (for with-bloom rendering path) */
    if (!create_pre_bloom_fbo(d)) return false;

    fprintf(stderr, "[hummingbird] Initialized: %dx%d, bloom %dx%d\n",
            d->width, d->height, d->bloom_w, d->bloom_h);
    return true;
}

static void hummingbird_update(Layer *self, const AudioBands *bands, float dt) {
    (void)bands;
    HummingbirdData *d = (HummingbirdData *)self->user_data;
    d->stashed_dt = dt;
}

static void hummingbird_draw(Layer *self, const AudioBands *bands) {
    HummingbirdData *d = (HummingbirdData *)self->user_data;

    if (self->current_option == 1) {
        hummingbird_draw_no_bloom(d, bands);
    } else if (self->current_option == 2) {
        hummingbird_draw_with_bloom(d, bands);
    }
}

static void hummingbird_resize(Layer *self, int fb_width, int fb_height) {
    HummingbirdData *d = (HummingbirdData *)self->user_data;
    d->width  = fb_width;
    d->height = fb_height;
    d->bloom_w = fb_width  / BLOOM_DOWNSAMPLE;
    d->bloom_h = fb_height / BLOOM_DOWNSAMPLE;
    /* TODO: Recreate FBOs at new size */
    fprintf(stderr, "[hummingbird] Resize: %dx%d\n", fb_width, fb_height);
}

static void hummingbird_shutdown(Layer *self) {
    HummingbirdData *d = (HummingbirdData *)self->user_data;
    if (!d) return;

    if (d->scene_prog)   glDeleteProgram(d->scene_prog);
    if (d->extract_prog) glDeleteProgram(d->extract_prog);
    if (d->blur_prog)    glDeleteProgram(d->blur_prog);

    glDeleteFramebuffers(2, d->fbo_bloom);
    glDeleteTextures(2, d->tex_bloom);

    if (d->fbo_pre_bloom) glDeleteFramebuffers(1, &d->fbo_pre_bloom);
    if (d->tex_pre_bloom) glDeleteTextures(1, &d->tex_pre_bloom);
    if (d->rbo_pre_bloom_depth) glDeleteRenderbuffers(1, &d->rbo_pre_bloom_depth);

    if (d->model) model_destroy(d->model);

    free(d);
    self->user_data = NULL;

    fprintf(stderr, "[hummingbird] Shutdown\n");
}

/* ------------------------------------------------------------------ */
/* Public constructor                                                  */
/* ------------------------------------------------------------------ */

Layer *layer_hummingbird_create(void) {
    Layer *layer = calloc(1, sizeof(Layer));
    if (!layer) return NULL;

    HummingbirdData *d = calloc(1, sizeof(HummingbirdData));
    if (!d) {
        free(layer);
        return NULL;
    }

    layer->name         = "Hummingbird";
    layer->option_count = 2;
    layer->current_option = 0;  /* off by default, main.c sets to 1 */

    layer->position[0] = 0.0f;
    layer->position[1] = 0.0f;
    layer->position[2] = 0.0f;
    layer->rotation[0] = 0.0f;
    layer->rotation[1] = 0.0f;
    layer->rotation[2] = 0.0f;
    layer->scale[0] = 1.0f;
    layer->scale[1] = 1.0f;
    layer->scale[2] = 1.0f;

    layer->init     = hummingbird_init;
    layer->update   = hummingbird_update;
    layer->draw     = hummingbird_draw;
    layer->resize   = hummingbird_resize;
    layer->shutdown = hummingbird_shutdown;

    layer->user_data = d;

    return layer;
}
