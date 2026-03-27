/*
 * renderer.c — GL renderer with layer composition
 *
 * Pipeline:
 *   1. Iterate enabled layers, each draws into shared scene FBO
 *   2. Composite scene FBO to screen
 */
#include "renderer.h"
#include "shader.h"
#include "layer.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define MAX_LAYERS 16

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static int s_width, s_height;

/* Shaders */
static GLuint s_composite_prog = 0;

/* FBOs: scene */
static GLuint s_fbo_scene = 0, s_tex_scene = 0, s_rbo_depth = 0;

/* Fullscreen quad */
static GLuint s_quad_vbo = 0;

/* Layer registry */
static Layer *s_layers[MAX_LAYERS];
static int s_layer_count = 0;

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

void draw_fullscreen_quad(void) {
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

bool create_fbo_color(GLuint *fbo, GLuint *tex, int w, int h) {
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
    s_composite_prog = shader_load(
        SHADER_PATH("fullscreen.vert"), SHADER_PATH("composite.frag"));
    if (!s_composite_prog) {
        fprintf(stderr, "[renderer] Failed to load composite shader\n");
        return false;
    }
    glBindAttribLocation(s_composite_prog, 0, "a_position");
    glBindAttribLocation(s_composite_prog, 1, "a_texcoord");
    glLinkProgram(s_composite_prog);
    return true;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

bool renderer_init(int width, int height) {
    s_width  = width;
    s_height = height;

    fprintf(stderr, "[renderer] Init: %dx%d\n", s_width, s_height);

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

    /* Shaders */
    if (!load_shaders()) return false;

    fprintf(stderr, "[renderer] Initialized successfully\n");
    return true;
}

bool renderer_add_layer(Layer *layer) {
    if (s_layer_count >= MAX_LAYERS) {
        fprintf(stderr, "[renderer] MAX_LAYERS (%d) reached\n", MAX_LAYERS);
        return false;
    }
    s_layers[s_layer_count++] = layer;
    fprintf(stderr, "[renderer] Added layer '%s' (index %d)\n",
            layer->name, s_layer_count - 1);
    return true;
}

int renderer_get_layer_count(void) {
    return s_layer_count;
}

Layer *renderer_get_layer(int index) {
    if (index < 0 || index >= s_layer_count) return NULL;
    return s_layers[index];
}

GLuint renderer_get_scene_fbo(void) {
    return s_fbo_scene;
}

void renderer_get_dimensions(int *width, int *height) {
    if (width)  *width  = s_width;
    if (height) *height = s_height;
}

void renderer_frame(const AudioBands *bands, float dt) {
    /* Pass 1: Render all enabled layers into shared scene FBO */
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo_scene);
    glViewport(0, 0, s_width, s_height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (int i = 0; i < s_layer_count; i++) {
        Layer *l = s_layers[i];
        if (l->current_option > 0) {
            l->update(l, bands, dt);
            l->draw(l, bands);
        }
    }

    /* Pass 2: Composite scene FBO to screen */
    glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, s_width, s_height);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(s_composite_prog);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_tex_scene);
    glUniform1i(glGetUniformLocation(s_composite_prog, "u_scene"), 0);

    /* Bind scene texture as placeholder bloom to avoid null texture on GLES2 */
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_tex_scene);
    glUniform1i(glGetUniformLocation(s_composite_prog, "u_bloom"), 1);
    glUniform1f(glGetUniformLocation(s_composite_prog, "u_bloom_intensity"), 0.0f);
    glUniform1f(glGetUniformLocation(s_composite_prog, "u_bass"), bands->bass);
    glUniform1f(glGetUniformLocation(s_composite_prog, "u_mid"), bands->mid);

    draw_fullscreen_quad();

    glEnable(GL_DEPTH_TEST);
}

void renderer_resize(int width, int height) {
    s_width  = width;
    s_height = height;
    fprintf(stderr, "[renderer] Resize: %dx%d\n", width, height);

    /* Notify all layers */
    for (int i = 0; i < s_layer_count; i++) {
        Layer *l = s_layers[i];
        if (l->resize) {
            l->resize(l, width, height);
        }
    }
}

void renderer_shutdown(void) {
    if (s_composite_prog) glDeleteProgram(s_composite_prog);

    glDeleteFramebuffers(1, &s_fbo_scene);
    glDeleteTextures(1, &s_tex_scene);
    glDeleteRenderbuffers(1, &s_rbo_depth);
    glDeleteBuffers(1, &s_quad_vbo);

    s_layer_count = 0;

    fprintf(stderr, "[renderer] Shutdown\n");
}
