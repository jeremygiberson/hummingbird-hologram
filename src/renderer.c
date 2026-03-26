/*
 * renderer.c — GL renderer with multi-pass bloom
 *
 * Pipeline:
 *   1. Render scene → FBO A (model on black)
 *   2. Bright extract: FBO A → FBO B (threshold)
 *   3. Gaussian blur: FBO B → FBO C → FBO B (ping-pong, 2 iterations)
 *   4. Composite: FBO A (scene) + FBO B (bloom) → screen
 */
#include "renderer.h"
#include "shader.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Bloom configuration                                                 */
/* ------------------------------------------------------------------ */

#define BLOOM_BLUR_PASSES   3       /* Number of blur ping-pong iterations */
#define BLOOM_DOWNSAMPLE    2       /* Bloom FBOs at 1/N resolution */
#define BLOOM_THRESHOLD     0.7f    /* Brightness cutoff for extraction */

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static int s_width, s_height;
static int s_bloom_w, s_bloom_h;

/* Shaders */
static GLuint s_scene_prog     = 0;
static GLuint s_extract_prog   = 0;
static GLuint s_blur_prog      = 0;
static GLuint s_composite_prog = 0;

/* FBOs: A = scene, B/C = bloom ping-pong */
static GLuint s_fbo_scene = 0, s_tex_scene = 0, s_rbo_depth = 0;
static GLuint s_fbo_bloom[2] = {0}, s_tex_bloom[2] = {0};

/* Fullscreen quad */
static GLuint s_quad_vbo = 0;

/* Scene shader uniforms */
static ModelUniforms s_model_uniforms;

/* ------------------------------------------------------------------ */
/* Fullscreen quad                                                     */
/* ------------------------------------------------------------------ */

static const float QUAD_VERTS[] = {
    /* pos (x,y), texcoord (u,v) */
    -1.0f, -1.0f,  0.0f, 0.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
    -1.0f,  1.0f,  0.0f, 1.0f,
     1.0f,  1.0f,  1.0f, 1.0f,
};

static void draw_fullscreen_quad(void) {
    glBindBuffer(GL_ARRAY_BUFFER, s_quad_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
}

/* ------------------------------------------------------------------ */
/* FBO creation                                                        */
/* ------------------------------------------------------------------ */

static bool create_fbo_color(GLuint *fbo, GLuint *tex, int w, int h) {
    glGenFramebuffers(1, fbo);
    glGenTextures(1, tex);

    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, *tex, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[renderer] FBO incomplete: 0x%x\n", status);
        return false;
    }
    return true;
}

static bool create_scene_fbo(int w, int h) {
    glGenFramebuffers(1, &s_fbo_scene);
    glGenTextures(1, &s_tex_scene);
    glGenRenderbuffers(1, &s_rbo_depth);

    /* Color attachment */
    glBindTexture(GL_TEXTURE_2D, s_tex_scene);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* Depth attachment */
    glBindRenderbuffer(GL_RENDERBUFFER, s_rbo_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);

    /* Assemble */
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo_scene);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, s_tex_scene, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_RENDERBUFFER, s_rbo_depth);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[renderer] Scene FBO incomplete: 0x%x\n", status);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Shader loading                                                      */
/* ------------------------------------------------------------------ */

static bool load_shaders(void) {
    s_scene_prog = shader_load(
        SHADER_PATH("scene.vert"), SHADER_PATH("scene.frag"));
    s_extract_prog = shader_load(
        SHADER_PATH("fullscreen.vert"), SHADER_PATH("bright_extract.frag"));
    s_blur_prog = shader_load(
        SHADER_PATH("fullscreen.vert"), SHADER_PATH("blur.frag"));
    s_composite_prog = shader_load(
        SHADER_PATH("fullscreen.vert"), SHADER_PATH("composite.frag"));

    if (!s_scene_prog || !s_extract_prog || !s_blur_prog || !s_composite_prog) {
        fprintf(stderr, "[renderer] Failed to load one or more shaders\n");
        return false;
    }

    /* Bind attribute locations BEFORE linking (must match model.c layout) */
    glBindAttribLocation(s_scene_prog, 0, "a_position");
    glBindAttribLocation(s_scene_prog, 1, "a_normal");
    glBindAttribLocation(s_scene_prog, 2, "a_texcoord");
    glBindAttribLocation(s_scene_prog, 3, "a_joints");
    glBindAttribLocation(s_scene_prog, 4, "a_weights");
    glLinkProgram(s_scene_prog);  /* Re-link after binding attribs */

    /* Cache scene shader uniform locations (after re-link) */
    s_model_uniforms.u_model_matrix = glGetUniformLocation(s_scene_prog, "u_model");
    s_model_uniforms.u_view_matrix  = glGetUniformLocation(s_scene_prog, "u_view");
    s_model_uniforms.u_proj_matrix  = glGetUniformLocation(s_scene_prog, "u_proj");
    s_model_uniforms.u_texture0     = glGetUniformLocation(s_scene_prog, "u_texture0");
    s_model_uniforms.u_texture1     = glGetUniformLocation(s_scene_prog, "u_texture1");
    s_model_uniforms.u_energy       = glGetUniformLocation(s_scene_prog, "u_energy");
    s_model_uniforms.u_has_skin     = glGetUniformLocation(s_scene_prog, "u_has_skin");

    /* Look up joint matrix uniforms: u_joints[0], u_joints[1], ... */
    for (int i = 0; i < MAX_JOINTS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "u_joints[%d]", i);
        s_model_uniforms.u_joints[i] = glGetUniformLocation(s_scene_prog, name);
    }

    /* Bind fullscreen quad attribute locations for post-process shaders */
    GLuint post_progs[] = {s_extract_prog, s_blur_prog, s_composite_prog};
    for (int i = 0; i < 3; i++) {
        glBindAttribLocation(post_progs[i], 0, "a_position");
        glBindAttribLocation(post_progs[i], 1, "a_texcoord");
        glLinkProgram(post_progs[i]);
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

bool renderer_init(int width, int height) {
    s_width  = width;
    s_height = height;
    s_bloom_w = width  / BLOOM_DOWNSAMPLE;
    s_bloom_h = height / BLOOM_DOWNSAMPLE;

    fprintf(stderr, "[renderer] Init: %dx%d, bloom %dx%d\n",
            s_width, s_height, s_bloom_w, s_bloom_h);

    /* Global GL state */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Fullscreen quad VBO */
    glGenBuffers(1, &s_quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD_VERTS), QUAD_VERTS, GL_STATIC_DRAW);

    /* FBOs */
    if (!create_scene_fbo(s_width, s_height)) return false;
    if (!create_fbo_color(&s_fbo_bloom[0], &s_tex_bloom[0], s_bloom_w, s_bloom_h))
        return false;
    if (!create_fbo_color(&s_fbo_bloom[1], &s_tex_bloom[1], s_bloom_w, s_bloom_h))
        return false;

    /* Shaders */
    if (!load_shaders()) return false;

    fprintf(stderr, "[renderer] Initialized successfully\n");
    return true;
}

void renderer_frame(Model *model, const AudioBands *bands, float dt) {
    /* --- Pass 1: Render scene to FBO --- */
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo_scene);
    glViewport(0, 0, s_width, s_height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(s_scene_prog);

    /* Set audio-reactive uniforms on scene shader */
    GLint loc;
    loc = glGetUniformLocation(s_scene_prog, "u_bass");
    if (loc >= 0) glUniform1f(loc, bands->bass);
    loc = glGetUniformLocation(s_scene_prog, "u_mid");
    if (loc >= 0) glUniform1f(loc, bands->mid);
    loc = glGetUniformLocation(s_scene_prog, "u_high");
    if (loc >= 0) glUniform1f(loc, bands->high);

    model_update(model, dt);
    model_draw(model, &s_model_uniforms, bands->energy);

    /* --- Pass 2: Bright extract --- */
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo_bloom[0]);
    glViewport(0, 0, s_bloom_w, s_bloom_h);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);

    glUseProgram(s_extract_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_tex_scene);
    glUniform1i(glGetUniformLocation(s_extract_prog, "u_scene"), 0);
    glUniform1f(glGetUniformLocation(s_extract_prog, "u_threshold"),
                BLOOM_THRESHOLD - bands->bass * 0.3f);  /* Bass lowers threshold */

    draw_fullscreen_quad();

    /* --- Pass 3: Gaussian blur ping-pong --- */
    glUseProgram(s_blur_prog);
    GLint u_image     = glGetUniformLocation(s_blur_prog, "u_image");
    GLint u_horizontal = glGetUniformLocation(s_blur_prog, "u_horizontal");
    GLint u_texel_size = glGetUniformLocation(s_blur_prog, "u_texel_size");

    glUniform2f(u_texel_size, 1.0f / (float)s_bloom_w, 1.0f / (float)s_bloom_h);

    for (int i = 0; i < BLOOM_BLUR_PASSES; i++) {
        /* Horizontal: bloom[0] → bloom[1] */
        glBindFramebuffer(GL_FRAMEBUFFER, s_fbo_bloom[1]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_tex_bloom[0]);
        glUniform1i(u_image, 0);
        glUniform1i(u_horizontal, 1);
        draw_fullscreen_quad();

        /* Vertical: bloom[1] → bloom[0] */
        glBindFramebuffer(GL_FRAMEBUFFER, s_fbo_bloom[0]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_tex_bloom[1]);
        glUniform1i(u_horizontal, 0);
        draw_fullscreen_quad();
    }

    /* --- Pass 4: Composite to screen --- */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, s_width, s_height);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(s_composite_prog);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_tex_scene);
    glUniform1i(glGetUniformLocation(s_composite_prog, "u_scene"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_tex_bloom[0]);
    glUniform1i(glGetUniformLocation(s_composite_prog, "u_bloom"), 1);

    /* Audio-reactive bloom intensity */
    float bloom_intensity = 1.0f + bands->bass * 2.0f;
    glUniform1f(glGetUniformLocation(s_composite_prog, "u_bloom_intensity"),
                bloom_intensity);
    glUniform1f(glGetUniformLocation(s_composite_prog, "u_bass"), bands->bass);
    glUniform1f(glGetUniformLocation(s_composite_prog, "u_mid"), bands->mid);

    draw_fullscreen_quad();

    glEnable(GL_DEPTH_TEST);
}

void renderer_resize(int width, int height) {
    /* TODO: Recreate FBOs at new size */
    s_width  = width;
    s_height = height;
    s_bloom_w = width  / BLOOM_DOWNSAMPLE;
    s_bloom_h = height / BLOOM_DOWNSAMPLE;
    fprintf(stderr, "[renderer] Resize: %dx%d\n", width, height);
}

void renderer_shutdown(void) {
    if (s_scene_prog)     glDeleteProgram(s_scene_prog);
    if (s_extract_prog)   glDeleteProgram(s_extract_prog);
    if (s_blur_prog)      glDeleteProgram(s_blur_prog);
    if (s_composite_prog) glDeleteProgram(s_composite_prog);

    glDeleteFramebuffers(1, &s_fbo_scene);
    glDeleteTextures(1, &s_tex_scene);
    glDeleteRenderbuffers(1, &s_rbo_depth);
    glDeleteFramebuffers(2, s_fbo_bloom);
    glDeleteTextures(2, s_tex_bloom);
    glDeleteBuffers(1, &s_quad_vbo);

    fprintf(stderr, "[renderer] Shutdown\n");
}
