/*
 * renderer.h — GL renderer with bloom pipeline
 */
#pragma once

#include "platform.h"
#include "audio.h"
#include "model.h"

#include <stdbool.h>

/* Initialize the renderer: create FBOs, load shaders, set up fullscreen quad.
 * Must be called after GL context is created.
 * width/height are the framebuffer dimensions. */
bool renderer_init(int width, int height);

/* Render one frame. */
void renderer_frame(Model *model, const AudioBands *bands, float dt);

/* Handle window resize / display mode change. */
void renderer_resize(int width, int height);

/* Cleanup all GL resources. */
void renderer_shutdown(void);
