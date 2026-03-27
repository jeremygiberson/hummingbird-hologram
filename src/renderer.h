/*
 * renderer.h — GL renderer with layer composition
 */
#pragma once

#include "platform.h"
#include "audio.h"
#include "layer.h"

#include <stdbool.h>

/* Initialize the renderer: create shared scene FBO, composite shader,
 * fullscreen quad. Must be called after GL context is created. */
bool renderer_init(int width, int height);

/* Register a layer. Order of registration = draw order (back to front).
 * Returns false if MAX_LAYERS reached. */
bool renderer_add_layer(Layer *layer);

/* Get the number of registered layers (for key handling in main.c). */
int renderer_get_layer_count(void);

/* Get a registered layer by index. Returns NULL if out of range. */
Layer *renderer_get_layer(int index);

/* Render one frame: iterate enabled layers, composite to screen. */
void renderer_frame(const AudioBands *bands, float dt);

/* Handle window resize / display mode change. */
void renderer_resize(int width, int height);

/* Cleanup all GL resources. Does NOT shut down layers — caller does that. */
void renderer_shutdown(void);

/* --- Shared utilities for layers --- */

/* Returns the shared scene FBO handle. Layers that bind their own FBOs
 * must rebind this before returning from draw(). */
GLuint renderer_get_scene_fbo(void);

/* Query current framebuffer dimensions. */
void renderer_get_dimensions(int *width, int *height);

/* Draw a fullscreen quad using the renderer's shared VBO.
 * Binds attrib 0 = position (vec2), attrib 1 = texcoord (vec2).
 * Caller must have already bound their shader program. */
void draw_fullscreen_quad(void);

/* Create a color-only FBO (no depth). Useful for bloom ping-pong buffers.
 * Caller owns the returned fbo and tex handles. */
bool create_fbo_color(GLuint *fbo, GLuint *tex, int w, int h);
