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
#include <math.h>

/* ------------------------------------------------------------------ */
/* Bloom configuration                                                 */
/* ------------------------------------------------------------------ */

#define BLOOM_BLUR_PASSES   3       /* Number of blur ping-pong iterations */
#define BLOOM_DOWNSAMPLE    2       /* Bloom FBOs at 1/N resolution */
#define BLOOM_THRESHOLD     0.85f   /* Brightness cutoff for extraction */
#define MOTION_BLUR_SAMPLES 1       /* 1 = disabled for now */

/* --- BPM tracking --- */
#define BPM_HISTORY         16      /* Number of beat intervals to average */
#define WING_SPEED_DEFAULT  2.3f    /* Wing speed when no BPM detected */
#define WING_SPEED_MIN      1.5f    /* Slowest wing flap */
#define WING_SPEED_MAX      6.0f    /* Fastest wing flap */
#define BPM_MIN             60.0f
#define BPM_MAX             180.0f

/* --- Orientation change --- */
#define POSE_EASE_DURATION  1.0f    /* Seconds to ease between poses */
#define POSE_COOLDOWN       2.0f    /* Minimum seconds between pose changes */
#define POSE_CHANGE_THRESH  0.06f   /* Spectral divergence threshold to trigger */
#define POSE_SLOW_EMA       0.005f  /* Slow EMA alpha (~8s window at 30fps) */
/* Base transform already has -90° Y rotation (beak pointing right).
 * extra_rotation_y is additive:
 *   -0.4 rad (~-23°) → more rightward (looking further right)
 *    0.0 rad         → side profile (as loaded)
 *   +1.57 rad (+90°) → facing camera (forward)
 * Range: right profile → forward-ish */
#define ROTATION_Y_MIN    0.4f// (-0.4f)  /* Slightly more rightward */
#define ROTATION_Y_MAX     2.4f * 1.57f   /* Facing camera (forward) */
#define PITCH_X_MIN        (-0.3f)  /* Nose down (~-17°) */
#define PITCH_X_MAX         0.3f    /* Nose up (~+17°) */
#define SCALE_MIN           0.55f
#define SCALE_MAX           1.1f
#define TRANSLATE_RANGE     1.7f    /* Max translation offset in each axis */

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

    /* --- BPM tracking --- */
    float beat_intervals[BPM_HISTORY]; /* Seconds between consecutive beats */
    int   beat_interval_idx;
    int   beat_interval_count;         /* How many intervals we've recorded */
    float time_since_last_beat;
    bool  beat_was_high;               /* Was beat > threshold last frame? */
    float current_bpm;                 /* Smoothed BPM estimate */

    /* --- Pose easing --- */
    float pose_cooldown_timer;         /* Counts down after a pose change */

    /* Current interpolated values (the "live" state) */
    float cur_rotation_y;
    float cur_rotation_x;  /* Pitch */
    float cur_scale;
    float cur_translation[3];

    /* Ease source (where we started easing from) */
    float src_rotation_y;
    float src_rotation_x;
    float src_scale;
    float src_translation[3];

    /* Ease target (where we're easing to) */
    float tgt_rotation_y;
    float tgt_rotation_x;
    float tgt_scale;
    float tgt_translation[3];

    float ease_elapsed;   /* Time spent easing so far */
    bool  easing;         /* True when an ease is in progress */

    /* --- Spectral shift detection --- */
    float slow_bass;               /* Slow EMA of bass (~8s window) */
    float slow_mid;
    float slow_centroid;
    float slow_energy;
    bool  slow_initialized;        /* False until first frame */

    /* RNG state (simple LCG for pose randomization) */
    unsigned int rng_state;
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
/* BPM & pose helpers                                                  */
/* ------------------------------------------------------------------ */

static float hb_randf(HummingbirdData *d) {
    /* Simple LCG PRNG — good enough for random pose selection */
    d->rng_state = d->rng_state * 1103515245u + 12345u;
    return (float)((d->rng_state >> 16) & 0x7FFF) / 32767.0f;
}

static float hb_randf_range(HummingbirdData *d, float lo, float hi) {
    return lo + hb_randf(d) * (hi - lo);
}

/* Attempt at Apple-like smoothstep easing (ease-in-out) */
static float smoothstep_ease(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

static void update_bpm(HummingbirdData *d, const AudioBands *bands, float dt) {
    d->time_since_last_beat += dt;

    /* Detect rising edge of beat signal */
    bool beat_high = bands->beat > 0.5f;
    if (beat_high && !d->beat_was_high) {
        /* Record interval if we have a previous beat */
        if (d->time_since_last_beat < 3.0f && d->time_since_last_beat > 0.15f) {
            d->beat_intervals[d->beat_interval_idx] = d->time_since_last_beat;
            d->beat_interval_idx = (d->beat_interval_idx + 1) % BPM_HISTORY;
            if (d->beat_interval_count < BPM_HISTORY)
                d->beat_interval_count++;
        }
        d->time_since_last_beat = 0.0f;
    }
    d->beat_was_high = beat_high;

    /* Compute BPM from rolling average of intervals */
    if (d->beat_interval_count >= 3) {
        float sum = 0.0f;
        for (int i = 0; i < d->beat_interval_count; i++)
            sum += d->beat_intervals[i];
        float avg_interval = sum / (float)d->beat_interval_count;
        float raw_bpm = 60.0f / avg_interval;
        /* Smooth BPM changes */
        d->current_bpm = lerpf(d->current_bpm, raw_bpm, 0.1f);
    }

    /* Map BPM to wing speed */
    float wing_speed;
    if (d->current_bpm > 0.0f) {
        float t = (d->current_bpm - BPM_MIN) / (BPM_MAX - BPM_MIN);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        wing_speed = WING_SPEED_MIN + t * (WING_SPEED_MAX - WING_SPEED_MIN);
    } else {
        wing_speed = WING_SPEED_DEFAULT;
    }

    /* Decay BPM toward 0 if no beats for a while */
    if (d->time_since_last_beat > 5.0f) {
        d->current_bpm *= 0.95f;
        if (d->current_bpm < 1.0f) d->current_bpm = 0.0f;
        d->beat_interval_count = 0;
    }

    model_set_wing_speed(d->model, wing_speed);
}

static void trigger_new_pose(HummingbirdData *d) {
    /* Snapshot current interpolated values as ease source */
    d->src_rotation_y = d->cur_rotation_y;
    d->src_rotation_x = d->cur_rotation_x;
    d->src_scale = d->cur_scale;
    d->src_translation[0] = d->cur_translation[0];
    d->src_translation[1] = d->cur_translation[1];
    d->src_translation[2] = d->cur_translation[2];

    /* Pick new random targets */
    d->tgt_rotation_y = hb_randf_range(d, ROTATION_Y_MIN, ROTATION_Y_MAX);
    d->tgt_rotation_x = hb_randf_range(d, PITCH_X_MIN, PITCH_X_MAX);
    d->tgt_scale = hb_randf_range(d, SCALE_MIN, SCALE_MAX);
    d->tgt_translation[0] = hb_randf_range(d, -TRANSLATE_RANGE, TRANSLATE_RANGE);
    d->tgt_translation[1] = hb_randf_range(d, -TRANSLATE_RANGE * 0.5f, TRANSLATE_RANGE * 0.5f);
    d->tgt_translation[2] = 0.0f;  /* Don't move toward/away from camera */

    d->ease_elapsed = 0.0f;
    d->easing = true;
    d->pose_cooldown_timer = POSE_COOLDOWN;

    fprintf(stderr, "[hummingbird] New pose: rot=%.2f scale=%.2f trans=(%.2f,%.2f)\n",
            d->tgt_rotation_y, d->tgt_scale,
            d->tgt_translation[0], d->tgt_translation[1]);
}

static void update_pose(HummingbirdData *d, const AudioBands *bands, float dt) {
    /* Decrement cooldown */
    if (d->pose_cooldown_timer > 0.0f)
        d->pose_cooldown_timer -= dt;

    /* --- Spectral shift detection ---
     * Track slow-moving averages of key audio features (~8s window).
     * When current values diverge significantly from the slow average,
     * the musical character has changed (new section, instrument, key, etc). */
    if (!d->slow_initialized) {
        d->slow_bass     = bands->bass;
        d->slow_mid      = bands->mid;
        d->slow_centroid  = bands->spectral_centroid;
        d->slow_energy   = bands->energy;
        d->slow_initialized = true;
    } else {
        d->slow_bass     += POSE_SLOW_EMA * (bands->bass - d->slow_bass);
        d->slow_mid      += POSE_SLOW_EMA * (bands->mid - d->slow_mid);
        d->slow_centroid += POSE_SLOW_EMA * (bands->spectral_centroid - d->slow_centroid);
        d->slow_energy   += POSE_SLOW_EMA * (bands->energy - d->slow_energy);
    }

    /* Compute divergence: how different is the current moment from recent history */
    float div_bass     = fabsf(bands->bass - d->slow_bass);
    float div_mid      = fabsf(bands->mid - d->slow_mid);
    float div_centroid = fabsf(bands->spectral_centroid - d->slow_centroid);
    float div_energy   = fabsf(bands->energy - d->slow_energy);
    float divergence   = (div_bass + div_mid + div_centroid + div_energy) * 0.25f;

    if (d->pose_cooldown_timer <= 0.0f && divergence > POSE_CHANGE_THRESH) {
        trigger_new_pose(d);
    }

    /* Advance easing */
    if (d->easing) {
        d->ease_elapsed += dt;
        float t = smoothstep_ease(d->ease_elapsed / POSE_EASE_DURATION);

        d->cur_rotation_y = lerpf(d->src_rotation_y, d->tgt_rotation_y, t);
        d->cur_rotation_x = lerpf(d->src_rotation_x, d->tgt_rotation_x, t);
        d->cur_scale = lerpf(d->src_scale, d->tgt_scale, t);
        d->cur_translation[0] = lerpf(d->src_translation[0], d->tgt_translation[0], t);
        d->cur_translation[1] = lerpf(d->src_translation[1], d->tgt_translation[1], t);
        d->cur_translation[2] = lerpf(d->src_translation[2], d->tgt_translation[2], t);

        if (d->ease_elapsed >= POSE_EASE_DURATION)
            d->easing = false;
    }

    /* Apply to model */
    model_set_extra_transform(d->model, d->cur_rotation_y, d->cur_rotation_x,
                               d->cur_scale, d->cur_translation);
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
    HummingbirdData *d = (HummingbirdData *)self->user_data;
    d->stashed_dt = dt;

    update_bpm(d, bands, dt);
    update_pose(d, bands, dt);
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

    /* Initialize pose easing state */
    d->cur_scale = 1.0f;
    d->tgt_scale = 1.0f;
    d->src_scale = 1.0f;
    d->rng_state = (unsigned int)SDL_GetTicks();  /* Seed from time */

    /* Trigger an initial random pose */
    trigger_new_pose(d);

    return layer;
}
