# Layer System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor the monolithic renderer into a composable layer system, extracting the hummingbird rendering into the first layer with bloom as a togglable option.

**Architecture:** Each layer is a separate `.c/.h` file implementing a vtable interface defined in `layer.h`. `renderer.c` iterates enabled layers each frame, calling `update` then `draw`. Layers draw into a shared scene FBO; the renderer composites the result to screen. Key presses 1-0 toggle layers on/off and cycle options.

**Tech Stack:** C11, OpenGL ES 2.0 / GL 2.1, SDL2, CMake

**Spec:** `docs/superpowers/specs/2026-03-26-layer-system-design.md`

---

## File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `src/layer.h` | CREATE | Layer vtable struct definition |
| `src/layer_hummingbird.h` | CREATE | `layer_hummingbird_create()` declaration |
| `src/layer_hummingbird.c` | CREATE | Hummingbird init/update/draw/resize/shutdown — extracted from renderer.c |
| `src/renderer.h` | MODIFY | Add layer API, expose utilities, change `renderer_frame` signature |
| `src/renderer.c` | MODIFY | Strip model-specific code, add layer iteration loop |
| `src/main.c` | MODIFY | Layer creation/registration, key handling, updated shutdown |
| `CMakeLists.txt` | MODIFY | Add `layer_hummingbird.c` to SOURCES |
| `CLAUDE.md` | MODIFY | Add Layer System section |

---

### Task 1: Create `src/layer.h`

**Files:**
- Create: `src/layer.h`

- [ ] **Step 1: Write the Layer interface header**

```c
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
```

- [ ] **Step 2: Verify it compiles**

Run from build dir:
```bash
cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug 2>&1 | tail -5
```
Expected: CMake succeeds (header is not yet included anywhere, so no compile test yet — just ensure no syntax issues when we include it in the next task).

- [ ] **Step 3: Commit**

```bash
git add src/layer.h
git commit -m "Add layer.h: composable rendering layer interface"
```

---

### Task 2: Update `src/renderer.h` — new public API

**Files:**
- Modify: `src/renderer.h`

- [ ] **Step 1: Update renderer.h with layer API and exposed utilities**

Replace the entire contents of `src/renderer.h` with:

```c
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
```

- [ ] **Step 2: Do NOT commit yet** — renderer.h, renderer.c, layer_hummingbird.c/.h, main.c, and CMakeLists.txt form an atomic unit. We commit them all together at the end of Task 6.

---

### Task 3: Refactor `src/renderer.c` — strip model code, add layer loop

**Files:**
- Modify: `src/renderer.c`

This is the largest task. `renderer.c` sheds all model-specific rendering (scene shader, bloom shaders, `ModelUniforms`, bloom FBOs) and keeps only: shared scene FBO, composite-to-screen, fullscreen quad, and layer iteration.

- [ ] **Step 1: Remove model-specific includes and state**

Remove `#include "model.h"` from the includes (line 1 area — it's pulled in via renderer.h currently, but renderer.h no longer includes model.h).

Remove these static variables (lines 32-46):
- `s_scene_prog` (line 33)
- `s_extract_prog` (line 34)
- `s_blur_prog` (line 35)
- `s_fbo_bloom[2]`, `s_tex_bloom[2]` (line 40)
- `s_model_uniforms` (line 46)

Keep:
- `s_width`, `s_height` (line 29)
- `s_bloom_w`, `s_bloom_h` — REMOVE these too (bloom is now layer-owned)
- `s_composite_prog` (line 36) — keep, renderer still composites to screen
- `s_fbo_scene`, `s_tex_scene`, `s_rbo_depth` (line 39) — keep, shared scene FBO
- `s_quad_vbo` (line 43) — keep, shared fullscreen quad

Add layer array state:

```c
#define MAX_LAYERS 10
static Layer *s_layers[MAX_LAYERS];
static int s_layer_count = 0;
```

- [ ] **Step 2: Make `draw_fullscreen_quad` non-static**

Change `static void draw_fullscreen_quad(void)` (line 60) to `void draw_fullscreen_quad(void)`. No other changes to this function.

- [ ] **Step 3: Remove `create_fbo_color` helper**

Delete the `create_fbo_color` function (lines 76-100). This moves to `layer_hummingbird.c`. Keep `create_scene_fbo` — it creates the shared scene FBO.

Actually — `create_fbo_color` is a general utility that any layer with FBOs would need. Instead of deleting it, make it non-static and declare it in `renderer.h`:

In `renderer.c`: change `static bool create_fbo_color(...)` to `bool create_fbo_color(...)`.

In `renderer.h`, add after the `draw_fullscreen_quad` declaration:

```c
/* Create a color-only FBO (no depth). Useful for bloom ping-pong buffers.
 * Caller owns the returned fbo and tex handles. */
bool create_fbo_color(GLuint *fbo, GLuint *tex, int w, int h);
```

- [ ] **Step 4: Replace `load_shaders` with minimal composite-only loader**

Replace the entire `load_shaders` function (lines 141-189) with:

```c
static bool load_shaders(void) {
    s_composite_prog = shader_load(
        SHADER_PATH("fullscreen.vert"), SHADER_PATH("composite.frag"));

    if (!s_composite_prog) {
        fprintf(stderr, "[renderer] Failed to load composite shader\n");
        return false;
    }

    /* Bind fullscreen quad attribute locations */
    glBindAttribLocation(s_composite_prog, 0, "a_position");
    glBindAttribLocation(s_composite_prog, 1, "a_texcoord");
    glLinkProgram(s_composite_prog);

    return true;
}
```

- [ ] **Step 5: Simplify `renderer_init`**

Remove bloom FBO creation from `renderer_init` (lines 195-227). The new version:

```c
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

    /* Shared scene FBO */
    if (!create_scene_fbo(s_width, s_height)) return false;

    /* Composite shader (scene FBO -> screen) */
    if (!load_shaders()) return false;

    fprintf(stderr, "[renderer] Initialized successfully\n");
    return true;
}
```

- [ ] **Step 6: Add layer management functions**

After `renderer_init`, add:

```c
bool renderer_add_layer(Layer *layer) {
    if (s_layer_count >= MAX_LAYERS) {
        fprintf(stderr, "[renderer] Cannot add layer '%s': max %d layers\n",
                layer->name, MAX_LAYERS);
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
```

- [ ] **Step 7: Rewrite `renderer_frame` with layer iteration**

Replace the entire `renderer_frame` function (lines 229-293) with:

```c
void renderer_frame(const AudioBands *bands, float dt) {
    /* --- Pass 1: Render all enabled layers into shared scene FBO --- */
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo_scene);
    glViewport(0, 0, s_width, s_height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Establish GL state contract for layers */
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

    /* --- Pass 2: Composite scene FBO to screen --- */
    glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, s_width, s_height);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(s_composite_prog);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_tex_scene);
    glUniform1i(glGetUniformLocation(s_composite_prog, "u_scene"), 0);

    /* Bloom disabled at renderer level — layers do their own bloom.
     * Bind scene texture as placeholder to avoid sampling null on GLES2. */
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_tex_scene);
    glUniform1i(glGetUniformLocation(s_composite_prog, "u_bloom"), 1);
    glUniform1f(glGetUniformLocation(s_composite_prog, "u_bloom_intensity"), 0.0f);
    glUniform1f(glGetUniformLocation(s_composite_prog, "u_bass"), bands->bass);
    glUniform1f(glGetUniformLocation(s_composite_prog, "u_mid"), bands->mid);

    draw_fullscreen_quad();

    glEnable(GL_DEPTH_TEST);
}
```

- [ ] **Step 8: Update `renderer_resize` to notify layers**

```c
void renderer_resize(int width, int height) {
    s_width  = width;
    s_height = height;
    fprintf(stderr, "[renderer] Resize: %dx%d\n", width, height);

    /* Notify all layers */
    for (int i = 0; i < s_layer_count; i++) {
        s_layers[i]->resize(s_layers[i], width, height);
    }
}
```

- [ ] **Step 9: Simplify `renderer_shutdown`**

Remove bloom shader/FBO cleanup. Keep only shared resources:

```c
void renderer_shutdown(void) {
    if (s_composite_prog) glDeleteProgram(s_composite_prog);

    glDeleteFramebuffers(1, &s_fbo_scene);
    glDeleteTextures(1, &s_tex_scene);
    glDeleteRenderbuffers(1, &s_rbo_depth);
    glDeleteBuffers(1, &s_quad_vbo);

    s_layer_count = 0;

    fprintf(stderr, "[renderer] Shutdown\n");
}
```

- [ ] **Step 10: Do NOT commit yet** — will not compile until main.c and CMakeLists.txt are updated. Continue to Task 4.

---

### Task 4: Create `src/layer_hummingbird.c` and `src/layer_hummingbird.h`

**Files:**
- Create: `src/layer_hummingbird.h`
- Create: `src/layer_hummingbird.c`

This extracts all model-specific rendering from the old `renderer.c` into a self-contained layer.

- [ ] **Step 1: Write `src/layer_hummingbird.h`**

```c
/*
 * layer_hummingbird.h — Hummingbird model rendering layer
 *
 * Option 1: Hummingbird without bloom
 * Option 2: Hummingbird with bloom
 */
#pragma once

#include "layer.h"

/* Create a hummingbird layer. Returns heap-allocated Layer with vtable set.
 * Caller must call layer->init(layer, w, h) before use.
 * On shutdown, layer->shutdown() frees user_data. Caller frees the Layer*. */
Layer *layer_hummingbird_create(void);
```

- [ ] **Step 2: Write `src/layer_hummingbird.c`**

This file contains the full hummingbird rendering extracted from `renderer.c`. The code below is the complete file:

```c
/*
 * layer_hummingbird.c — Hummingbird model rendering layer
 *
 * Extracted from renderer.c. Handles model loading, scene shader,
 * skeletal animation, motion blur, and optional bloom pipeline.
 */
#include "layer_hummingbird.h"
#include "renderer.h"
#include "shader.h"
#include "model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define BLOOM_BLUR_PASSES   3
#define BLOOM_DOWNSAMPLE    2
#define BLOOM_THRESHOLD     0.85f
#define MOTION_BLUR_SAMPLES 1       /* 1 = disabled */

/* ------------------------------------------------------------------ */
/* Layer-private state                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    Model *model;

    /* Scene shader */
    GLuint scene_prog;
    ModelUniforms scene_uniforms;

    /* Bloom pipeline (option 2 only) */
    GLuint extract_prog, blur_prog, composite_prog;
    GLuint fbo_pre_bloom, tex_pre_bloom, rbo_depth;
    GLuint fbo_bloom[2], tex_bloom[2];
    int fb_width, fb_height, bloom_width, bloom_height;

    /* dt stashed by update() for use in draw() (motion blur needs it) */
    float stashed_dt;
} HummingbirdData;

/* ------------------------------------------------------------------ */
/* FBO helpers                                                         */
/* ------------------------------------------------------------------ */

static bool create_pre_bloom_fbo(HummingbirdData *d) {
    glGenFramebuffers(1, &d->fbo_pre_bloom);
    glGenTextures(1, &d->tex_pre_bloom);
    glGenRenderbuffers(1, &d->rbo_depth);

    glBindTexture(GL_TEXTURE_2D, d->tex_pre_bloom);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, d->fb_width, d->fb_height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindRenderbuffer(GL_RENDERBUFFER, d->rbo_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
                          d->fb_width, d->fb_height);

    glBindFramebuffer(GL_FRAMEBUFFER, d->fbo_pre_bloom);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, d->tex_pre_bloom, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_RENDERBUFFER, d->rbo_depth);

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

    /* Bind attribute locations BEFORE re-linking */
    glBindAttribLocation(d->scene_prog, 0, "a_position");
    glBindAttribLocation(d->scene_prog, 1, "a_normal");
    glBindAttribLocation(d->scene_prog, 2, "a_texcoord");
    glBindAttribLocation(d->scene_prog, 3, "a_joints");
    glBindAttribLocation(d->scene_prog, 4, "a_weights");
    glLinkProgram(d->scene_prog);

    /* Cache uniform locations */
    d->scene_uniforms.u_model_matrix = glGetUniformLocation(d->scene_prog, "u_model");
    d->scene_uniforms.u_view_matrix  = glGetUniformLocation(d->scene_prog, "u_view");
    d->scene_uniforms.u_proj_matrix  = glGetUniformLocation(d->scene_prog, "u_proj");
    d->scene_uniforms.u_texture0     = glGetUniformLocation(d->scene_prog, "u_texture0");
    d->scene_uniforms.u_texture1     = glGetUniformLocation(d->scene_prog, "u_texture1");
    d->scene_uniforms.u_energy       = glGetUniformLocation(d->scene_prog, "u_energy");
    d->scene_uniforms.u_has_skin     = glGetUniformLocation(d->scene_prog, "u_has_skin");

    for (int i = 0; i < MAX_JOINTS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "u_joints[%d]", i);
        d->scene_uniforms.u_joints[i] = glGetUniformLocation(d->scene_prog, name);
    }

    return true;
}

static bool load_bloom_shaders(HummingbirdData *d) {
    d->extract_prog = shader_load(
        SHADER_PATH("fullscreen.vert"), SHADER_PATH("bright_extract.frag"));
    d->blur_prog = shader_load(
        SHADER_PATH("fullscreen.vert"), SHADER_PATH("blur.frag"));
    d->composite_prog = shader_load(
        SHADER_PATH("fullscreen.vert"), SHADER_PATH("composite.frag"));

    if (!d->extract_prog || !d->blur_prog || !d->composite_prog) {
        fprintf(stderr, "[hummingbird] Failed to load bloom shaders\n");
        return false;
    }

    /* Bind post-process attribute locations */
    GLuint progs[] = {d->extract_prog, d->blur_prog, d->composite_prog};
    for (int i = 0; i < 3; i++) {
        glBindAttribLocation(progs[i], 0, "a_position");
        glBindAttribLocation(progs[i], 1, "a_texcoord");
        glLinkProgram(progs[i]);
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* Vtable implementations                                              */
/* ------------------------------------------------------------------ */

static bool hummingbird_init(Layer *self, int fb_width, int fb_height) {
    HummingbirdData *d = (HummingbirdData *)self->user_data;
    d->fb_width  = fb_width;
    d->fb_height = fb_height;
    d->bloom_width  = fb_width  / BLOOM_DOWNSAMPLE;
    d->bloom_height = fb_height / BLOOM_DOWNSAMPLE;

    /* Load model */
    d->model = model_load(ASSET_PATH("hummingbird.glb"));
    if (!d->model) {
        fprintf(stderr, "[hummingbird] Model not found — using debug icosahedron\n");
        d->model = model_create_debug();
    }
    if (!d->model) {
        fprintf(stderr, "[hummingbird] Failed to create any model\n");
        return false;
    }

    /* Scene shader */
    if (!load_scene_shader(d)) return false;

    /* Bloom pipeline */
    if (!load_bloom_shaders(d)) return false;
    if (!create_pre_bloom_fbo(d)) return false;
    if (!create_fbo_color(&d->fbo_bloom[0], &d->tex_bloom[0],
                          d->bloom_width, d->bloom_height))
        return false;
    if (!create_fbo_color(&d->fbo_bloom[1], &d->tex_bloom[1],
                          d->bloom_width, d->bloom_height))
        return false;

    fprintf(stderr, "[hummingbird] Initialized (bloom FBOs: %dx%d)\n",
            d->bloom_width, d->bloom_height);
    return true;
}

static void render_model(HummingbirdData *d, const AudioBands *bands, float dt) {
    glUseProgram(d->scene_prog);

    /* Audio-reactive uniforms */
    GLint loc;
    loc = glGetUniformLocation(d->scene_prog, "u_bass");
    if (loc >= 0) glUniform1f(loc, bands->bass);
    loc = glGetUniformLocation(d->scene_prog, "u_mid");
    if (loc >= 0) glUniform1f(loc, bands->mid);
    loc = glGetUniformLocation(d->scene_prog, "u_high");
    if (loc >= 0) glUniform1f(loc, bands->high);

    GLint u_alpha = glGetUniformLocation(d->scene_prog, "u_alpha");
    float sub_dt = dt / (float)MOTION_BLUR_SAMPLES;
    float alpha = 1.0f / (float)MOTION_BLUR_SAMPLES;

    /* Additive blending for motion blur accumulation */
    glBlendFunc(GL_ONE, GL_ONE);

    for (int s = 0; s < MOTION_BLUR_SAMPLES; s++) {
        if (s > 0) glClear(GL_DEPTH_BUFFER_BIT);
        if (u_alpha >= 0) glUniform1f(u_alpha, alpha);

        model_update(d->model, sub_dt);
        model_draw(d->model, &d->scene_uniforms, bands->energy);
    }

    /* Restore standard blending */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static void hummingbird_draw_no_bloom(Layer *self, const AudioBands *bands, float dt) {
    HummingbirdData *d = (HummingbirdData *)self->user_data;

    /* Draw directly into the shared scene FBO (already bound by renderer) */
    int w, h;
    renderer_get_dimensions(&w, &h);
    glViewport(0, 0, w, h);

    render_model(d, bands, dt);
}

static void hummingbird_draw_with_bloom(Layer *self, const AudioBands *bands, float dt) {
    HummingbirdData *d = (HummingbirdData *)self->user_data;

    /* Step 1: Render model into private pre-bloom FBO */
    glBindFramebuffer(GL_FRAMEBUFFER, d->fbo_pre_bloom);
    glViewport(0, 0, d->fb_width, d->fb_height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    render_model(d, bands, dt);

    /* Step 2: Bright extract */
    glDisable(GL_DEPTH_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, d->fbo_bloom[0]);
    glViewport(0, 0, d->bloom_width, d->bloom_height);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(d->extract_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d->tex_pre_bloom);
    glUniform1i(glGetUniformLocation(d->extract_prog, "u_scene"), 0);
    glUniform1f(glGetUniformLocation(d->extract_prog, "u_threshold"), BLOOM_THRESHOLD);

    draw_fullscreen_quad();

    /* Step 3: Gaussian blur ping-pong */
    glUseProgram(d->blur_prog);
    GLint u_horizontal = glGetUniformLocation(d->blur_prog, "u_horizontal");
    GLint u_tex_blur   = glGetUniformLocation(d->blur_prog, "u_image");

    for (int pass = 0; pass < BLOOM_BLUR_PASSES; pass++) {
        /* Horizontal */
        glBindFramebuffer(GL_FRAMEBUFFER, d->fbo_bloom[1]);
        glClear(GL_COLOR_BUFFER_BIT);
        glUniform1i(u_horizontal, 1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, d->tex_bloom[0]);
        glUniform1i(u_tex_blur, 0);
        draw_fullscreen_quad();

        /* Vertical */
        glBindFramebuffer(GL_FRAMEBUFFER, d->fbo_bloom[0]);
        glClear(GL_COLOR_BUFFER_BIT);
        glUniform1i(u_horizontal, 0);
        glBindTexture(GL_TEXTURE_2D, d->tex_bloom[1]);
        draw_fullscreen_quad();
    }

    /* Step 4: Composite into shared scene FBO */
    glBindFramebuffer(GL_FRAMEBUFFER, renderer_get_scene_fbo());
    int fw, fh;
    renderer_get_dimensions(&fw, &fh);
    glViewport(0, 0, fw, fh);

    glBlendFunc(GL_ONE, GL_ONE);  /* Additive into scene */

    glUseProgram(d->composite_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d->tex_pre_bloom);
    glUniform1i(glGetUniformLocation(d->composite_prog, "u_scene"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, d->tex_bloom[0]);
    glUniform1i(glGetUniformLocation(d->composite_prog, "u_bloom"), 1);
    glUniform1f(glGetUniformLocation(d->composite_prog, "u_bloom_intensity"), 1.0f);
    glUniform1f(glGetUniformLocation(d->composite_prog, "u_bass"), bands->bass);
    glUniform1f(glGetUniformLocation(d->composite_prog, "u_mid"), bands->mid);

    draw_fullscreen_quad();

    /* Restore GL state contract */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
}

static void hummingbird_draw(Layer *self, const AudioBands *bands) {
    HummingbirdData *d = (HummingbirdData *)self->user_data;
    float dt = d->stashed_dt;

    if (self->current_option == 1) {
        hummingbird_draw_no_bloom(self, bands, dt);
    } else if (self->current_option == 2) {
        hummingbird_draw_with_bloom(self, bands, dt);
    }

    d->stashed_dt = 0.0f;
}

static void hummingbird_resize(Layer *self, int fb_width, int fb_height) {
    HummingbirdData *d = (HummingbirdData *)self->user_data;
    d->fb_width  = fb_width;
    d->fb_height = fb_height;
    d->bloom_width  = fb_width  / BLOOM_DOWNSAMPLE;
    d->bloom_height = fb_height / BLOOM_DOWNSAMPLE;

    /* TODO: Recreate bloom FBOs at new size */
}

static void hummingbird_shutdown(Layer *self) {
    HummingbirdData *d = (HummingbirdData *)self->user_data;
    if (!d) return;

    if (d->model) model_destroy(d->model);

    if (d->scene_prog)     glDeleteProgram(d->scene_prog);
    if (d->extract_prog)   glDeleteProgram(d->extract_prog);
    if (d->blur_prog)      glDeleteProgram(d->blur_prog);
    if (d->composite_prog) glDeleteProgram(d->composite_prog);

    glDeleteFramebuffers(1, &d->fbo_pre_bloom);
    glDeleteTextures(1, &d->tex_pre_bloom);
    glDeleteRenderbuffers(1, &d->rbo_depth);
    glDeleteFramebuffers(2, d->fbo_bloom);
    glDeleteTextures(2, d->tex_bloom);

    free(d);
    self->user_data = NULL;
}

static void hummingbird_update_impl(Layer *self, const AudioBands *bands, float dt) {
    HummingbirdData *d = (HummingbirdData *)self->user_data;
    (void)bands;
    d->stashed_dt = dt;
}

/* ------------------------------------------------------------------ */
/* Public: create                                                      */
/* ------------------------------------------------------------------ */

Layer *layer_hummingbird_create(void) {
    Layer *l = calloc(1, sizeof(Layer));
    if (!l) return NULL;

    HummingbirdData *d = calloc(1, sizeof(HummingbirdData));
    if (!d) { free(l); return NULL; }

    l->name         = "hummingbird";
    l->option_count = 2;        /* 1=no bloom, 2=bloom */
    l->current_option = 0;      /* starts off — caller sets to 1 */

    l->position[0] = 0.0f; l->position[1] = 0.0f; l->position[2] = 0.0f;
    l->rotation[0] = 0.0f; l->rotation[1] = 0.0f; l->rotation[2] = 0.0f;
    l->scale[0]    = 1.0f; l->scale[1]    = 1.0f; l->scale[2]    = 1.0f;

    l->init     = hummingbird_init;
    l->update   = hummingbird_update_impl;
    l->draw     = hummingbird_draw;
    l->resize   = hummingbird_resize;
    l->shutdown = hummingbird_shutdown;

    l->user_data = d;

    return l;
}
```

**Note on the `dt` problem:** The `Layer` vtable has `draw(self, bands)` without `dt`, but the hummingbird needs `dt` for motion blur sub-frames. The solution stashes `dt` in `HummingbirdData.stashed_dt` during `update()` and reads it in `draw()`. This is safe because update and draw are always called sequentially on the same thread. The stash lives in the instance data (not a file-static) so multiple hummingbird instances would work correctly.

**Note on per-layer transforms:** The `Layer.position/rotation/scale` fields are declared and initialized but NOT yet applied in the hummingbird draw path. This is intentional — no programmatic animation drives them yet. When animation is added later, the layer's draw code will need to compose `layer_transform * base_transform` as specified in the design spec.

- [ ] **Step 3: Do NOT commit yet** — continue to Task 5.

---

### Task 5: Update `src/main.c` — layer creation, key handling, shutdown

**Files:**
- Modify: `src/main.c`

- [ ] **Step 1: Update includes**

Replace:
```c
#include "renderer.h"
#include "audio.h"
#include "model.h"
```
With:
```c
#include "renderer.h"
#include "audio.h"
#include "layer.h"
#include "layer_hummingbird.h"
```

- [ ] **Step 2: Replace model loading with layer creation**

Replace lines 101-110 (the model loading block):
```c
    /* Load model */
    Model *model = model_load(ASSET_PATH("hummingbird.glb"));
    if (!model) {
        fprintf(stderr, "[main] Model not found — using debug icosahedron\n");
        model = model_create_debug();
    }
    if (!model) {
        fprintf(stderr, "[main] Failed to create any model\n");
        goto cleanup;
    }
```

With:
```c
    /* Create and register layers */
    Layer *hb = layer_hummingbird_create();
    if (!hb) {
        fprintf(stderr, "[main] Failed to create hummingbird layer\n");
        goto cleanup;
    }
    if (!hb->init(hb, fb_w, fb_h)) {
        fprintf(stderr, "[main] Hummingbird layer init failed\n");
        free(hb);
        goto cleanup;
    }
    hb->current_option = 1;  /* start enabled, no bloom */
    renderer_add_layer(hb);
```

- [ ] **Step 3: Add layer toggle key handling**

Replace the `SDL_KEYDOWN` case (lines 128-133):
```c
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_ESCAPE ||
                    ev.key.keysym.sym == SDLK_q) {
                    running = false;
                }
                break;
```

With:
```c
            case SDL_KEYDOWN: {
                SDL_Keycode sym = ev.key.keysym.sym;
                if (sym == SDLK_ESCAPE || sym == SDLK_q) {
                    running = false;
                } else {
                    /* Layer toggle: keys 1-9 -> indices 0-8, key 0 -> index 9 */
                    int idx = -1;
                    if (sym >= SDLK_1 && sym <= SDLK_9)
                        idx = sym - SDLK_1;
                    else if (sym == SDLK_0)
                        idx = 9;
                    if (idx >= 0 && idx < renderer_get_layer_count()) {
                        Layer *l = renderer_get_layer(idx);
                        if (l->current_option < l->option_count)
                            l->current_option++;
                        else
                            l->current_option = 0;
                        fprintf(stderr, "[main] Layer '%s' option: %d/%d\n",
                                l->name, l->current_option, l->option_count);
                    }
                }
                break;
            }
```

- [ ] **Step 4: Update render call**

Replace line 156:
```c
        renderer_frame(model, &bands, dt);
```

With:
```c
        renderer_frame(&bands, dt);
```

- [ ] **Step 5: Update cleanup section**

Replace lines 167-174:
```c
cleanup:
    fprintf(stderr, "[main] Shutting down\n");
    if (model) model_destroy(model);
    audio_shutdown();
    renderer_shutdown();
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
```

With:
```c
cleanup:
    fprintf(stderr, "[main] Shutting down\n");
    /* Shutdown and free all layers */
    for (int i = 0; i < renderer_get_layer_count(); i++) {
        Layer *l = renderer_get_layer(i);
        if (l) {
            l->shutdown(l);
            free(l);
        }
    }
    audio_shutdown();
    renderer_shutdown();
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
```

- [ ] **Step 6: Do NOT commit yet** — continue to Task 6.

---

### Task 6: Update `CMakeLists.txt`

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add layer_hummingbird.c to SOURCES**

In the `set(SOURCES ...)` block (lines 54-62), add `src/layer_hummingbird.c` after `src/renderer.c`:

```cmake
set(SOURCES
    src/main.c
    src/renderer.c
    src/layer_hummingbird.c
    src/shader.c
    src/audio.c
    src/model.c
    src/kiss_fft.c
    src/stb_impl.c
)
```

- [ ] **Step 2: Build and verify**

```bash
cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(sysctl -n hw.logicalcpu) 2>&1
```

Expected: Clean compile, zero errors, zero warnings (or only pre-existing warnings).

- [ ] **Step 3: Run and verify**

```bash
cd /Users/jeremygiberson/gitprojects/hologram && ./build/hologram &
sleep 3
# Press '1' key to verify toggle works (hummingbird should appear/disappear)
# Press Escape to quit
kill %1 2>/dev/null
```

Or use the screenshot script:
```bash
./scripts/screenshot.sh /tmp/layer_test.png
```

Expected: Hummingbird renders identically to before the refactor (option 1, no bloom).

- [ ] **Step 4: Commit all layer system changes atomically**

```bash
git add src/layer.h src/layer_hummingbird.h src/layer_hummingbird.c \
        src/renderer.h src/renderer.c src/main.c CMakeLists.txt
git commit -m "Refactor renderer into composable layer system

Extract hummingbird rendering into layer_hummingbird.c with vtable
interface defined in layer.h. renderer.c now iterates registered
layers and composites the shared scene FBO to screen. Keys 1-0
toggle layers on/off and cycle options. Hummingbird layer supports
option 1 (no bloom) and option 2 (with bloom)."
```

---

### Task 7: Test key toggling and bloom option

- [ ] **Step 1: Manual test — key toggle cycle**

Run the app and verify the key `1` cycles through:
1. First launch: hummingbird visible (option 1, no bloom — set by main.c)
2. Press `1`: option 2 (bloom enabled) — hummingbird should have glow effect
3. Press `1`: option 0 (off) — black screen
4. Press `1`: option 1 (no bloom) — hummingbird back, no glow

- [ ] **Step 2: Verify no GL errors**

Run with `MESA_DEBUG=1` or check stderr output for any GL error messages during option cycling.

- [ ] **Step 3: Verify clean exit**

Press Escape. Check stderr for the expected shutdown sequence:
```
[main] Layer 'hummingbird' option: ...
[main] Shutting down
[renderer] Shutdown
```

No crashes, no GL errors on cleanup.

---

### Task 8: Update `CLAUDE.md` — Layer System section

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Add Layer System section**

Add the following section to `CLAUDE.md` after the "Architecture" section (after the "Cross-Platform Strategy" subsection, before "Dependencies"):

```markdown
## Layer System

The rendering pipeline uses a composable layer architecture. Each layer is a self-contained rendering unit implemented as a separate `.c/.h` file with a vtable interface defined in `src/layer.h`.

### How It Works

- `renderer.c` manages a shared scene FBO and iterates registered layers each frame
- Each enabled layer's `update()` then `draw()` is called in registration order
- Layers draw into the shared scene FBO; the renderer composites the result to screen
- Keys `1`-`0` toggle layers on/off and cycle through options (dev mode)

### Current Layers

| Key | Layer | Options |
|-----|-------|---------|
| 1 | Hummingbird (`layer_hummingbird.c`) | 1: no bloom, 2: with bloom |

### Adding a New Layer

1. Create `src/layer_foo.h` and `src/layer_foo.c`
2. In the `.c` file, define a private data struct and implement the 5 vtable functions: `init`, `update`, `draw`, `resize`, `shutdown`
3. Implement a `Layer *layer_foo_create(void)` factory function that allocates the Layer and sets all vtable pointers
4. In `main.c`, call the factory, `init()`, set `current_option`, and `renderer_add_layer()`
5. Add the `.c` file to `CMakeLists.txt` SOURCES

### Adding Options to an Existing Layer

1. Increment `option_count` in the layer's `create()` function
2. Handle the new option value in `draw()` (switch/if on `self->current_option`)

### GL State Contract

When `draw()` is called, the renderer guarantees:
- Shared scene FBO is bound
- `GL_DEPTH_TEST` enabled
- `GL_BLEND` enabled with `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA`
- Color and depth already cleared

**Layers that change blend mode, disable depth test, or bind their own FBOs must restore all three before returning from `draw()`.**

### Shared Utilities Available to Layers

- `renderer_get_scene_fbo()` — rebind the shared FBO after using your own
- `draw_fullscreen_quad()` — draws a fullscreen quad (attrib 0=pos, 1=uv)
- `renderer_get_dimensions(&w, &h)` — current framebuffer size
- `create_fbo_color(&fbo, &tex, w, h)` — create a color-only FBO
- `shader_load(vert, frag)` — compile a shader program (from `shader.h`)

### Decision Prompt (for AI assistants)

When new rendering behavior is requested, clarify whether it is:
- **A new layer** — new visual element or effect, independent of existing layers
- **A new option on an existing layer** — variant rendering mode for an existing layer
- **A replacement** for an existing layer or option
- **A modification to the composition pipeline** — changes to `renderer.c`'s scene FBO or composite-to-screen pass

Do not assume. Ask.
```

- [ ] **Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "Add Layer System section to CLAUDE.md"
```

---

### Task 9: Final verification

- [ ] **Step 1: Clean build from scratch**

```bash
cd /Users/jeremygiberson/gitprojects/hologram
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(sysctl -n hw.logicalcpu) 2>&1
```

Expected: Clean compile.

- [ ] **Step 2: Run and screenshot**

```bash
cd /Users/jeremygiberson/gitprojects/hologram && ./scripts/screenshot.sh /tmp/hologram_layer_final.png
```

Expected: Hummingbird renders with same visual quality as before refactor.

- [ ] **Step 3: Verify git history is clean**

```bash
git log --oneline -10
git status
```

Expected: Series of focused commits, clean working tree.
