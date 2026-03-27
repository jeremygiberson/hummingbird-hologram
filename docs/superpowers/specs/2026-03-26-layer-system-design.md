# Layer System Design

## Overview

Refactor the monolithic rendering pipeline into a composable layer-based architecture. Each layer is a self-contained rendering unit with its own shaders, state, and animation. Layers are toggled on/off via keyboard (dev mode) or preset configs (appliance mode). The first layer is the existing hummingbird renderer, extracted from `renderer.c`.

## Requirements

1. **Up to 10 layers**, toggled via keys `1`-`0` (mapping to layer indices 0-9)
2. **Per-layer options**: pressing a layer's key cycles through its options, then turns it off
   - `current_option == 0` means off
   - Pressing the key: `0 -> 1 -> 2 -> ... -> option_count -> 0`
3. **Each layer receives audio analysis data** (`AudioBands`) for audio-reactive rendering
4. **Per-layer transform**: independent position, rotation, scale — writable by programmatic animation (keyframe interpolation, audio-driven transforms), NOT user-input driven
5. **Composition**: layers draw in registration order (back to front) into a shared scene FBO
6. **Presets** (future): a preset is a list of `{layer_index, option, position, rotation, scale}` tuples applied to the layer array. A single button cycles through presets.

## Architecture: Approach C — Separate Compilation Units

Each layer lives in its own `.c/.h` file. A common `layer.h` defines the interface. `renderer.c` iterates enabled layers and calls through the interface. Layers register themselves at startup via `main.c`.

## Layer Interface (`src/layer.h`)

```c
#ifndef LAYER_H
#define LAYER_H

#include "platform.h"
#include "audio.h"
#include <stdbool.h>

typedef struct Layer {
    const char *name;           // e.g., "hummingbird"
    int option_count;           // total options (0 = simple on/off, N = N modes)
    int current_option;         // 0 = off, 1..option_count = active option

    // Per-layer transform
    float position[3];          // world-space translation
    float rotation[3];          // euler angles (degrees), applied X->Y->Z
    float scale[3];             // per-axis scale (default 1,1,1)

    // Vtable
    bool (*init)(struct Layer *self, int fb_width, int fb_height);
    void (*update)(struct Layer *self, const AudioBands *bands, float dt);
    void (*draw)(struct Layer *self, const AudioBands *bands);
    void (*resize)(struct Layer *self, int fb_width, int fb_height);
    void (*shutdown)(struct Layer *self);

    void *user_data;            // layer-specific state (opaque)
} Layer;

#endif
```

### Field semantics

- **`option_count`**: Number of distinct modes. A layer with `option_count = 2` has two active modes (1 and 2) plus off (0). A layer with `option_count = 1` is simple on/off.
- **`position/rotation/scale`**: Set by programmatic animation in `update()`. Layers incorporate these into their model matrix. Initialized to `{0,0,0}`, `{0,0,0}`, `{1,1,1}`.
- **`init`**: Called once during startup. Allocate GL resources, load assets, compile shaders. Receives framebuffer dimensions.
- **`update`**: Called each frame (only for enabled layers). Advance animation, update transforms. Receives audio bands and delta time.
- **`draw`**: Called each frame (only for enabled layers, after update). Issue GL draw calls into the shared scene FBO. Receives audio bands for uniform setting. Responsible for setting its own audio-reactive uniforms. See "GL State Contract" below for entry/exit expectations.
- **`resize`**: Called when framebuffer dimensions change. Recreate any size-dependent resources (FBOs).
- **`shutdown`**: Called once during teardown. Free all GL resources, allocated memory, AND `self->user_data`. The layer owns its own cleanup.

## Scene Manager (in `renderer.c`)

### State

```c
#define MAX_LAYERS 10

static Layer *s_layers[MAX_LAYERS];
static int s_layer_count = 0;
```

### Registration

```c
// New public API
bool renderer_add_layer(Layer *layer);
```

Called from `main.c` after `renderer_init()`. Order of registration = draw order (back to front).

### Refactored `renderer_frame`

```
renderer_frame(bands, dt):
    1. Bind scene FBO, clear color + depth
    2. For each layer where current_option > 0:
        a. layer->update(layer, bands, dt)
        b. layer->draw(layer, bands)
    3. Bind default FBO (screen)
    4. Composite scene FBO to screen
    5. Swap
```

The composite pass remains in `renderer.c` — it just displays whatever is in the scene FBO. No per-model knowledge.

### Shared utilities exposed for layers

- `GLuint renderer_get_scene_fbo(void)` — returns the scene FBO handle so layers can bind it (or layers that do their own FBO work can bind back to it)
- `void draw_fullscreen_quad(void)` — made non-static, declared in `renderer.h`
- `void renderer_get_dimensions(int *w, int *h)` — so layers can query current framebuffer size
- Shader loading already in `shader.h/shader.c` — no change needed

### GL State Contract

The renderer establishes the following GL state before calling each layer's `draw()`:

- **Framebuffer**: shared scene FBO is bound
- **Depth test**: `GL_DEPTH_TEST` enabled
- **Blending**: `GL_BLEND` enabled with `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`
- **Clear**: color and depth already cleared for the frame

Layers that change blend mode, disable depth test, or bind their own FBOs **must restore all three** (FBO, depth, blend) before returning from `draw()`. Use `renderer_get_scene_fbo()` to rebind the shared FBO.

**Depth buffer is shared across layers** — it is NOT cleared between layer draw calls. This means 3D layers can occlude each other. For the Pepper's ghost use case (floating objects on black), this is generally desirable. If a future layer needs to ignore depth from prior layers, it can `glClear(GL_DEPTH_BUFFER_BIT)` at the start of its own `draw()`.

### Camera Ownership

**Each layer owns its own camera.** There is no shared view/projection matrix. The hummingbird layer uses the camera computed inside `model_load()` (auto-framed from bounding box). Future layers that render 3D content compute their own view/proj. Fullscreen shader layers (e.g., shadertoy effects) don't need a camera at all — they draw a fullscreen quad.

### Per-Layer Transform Composition

The `Layer.position/rotation/scale` fields compose with whatever internal transform a layer has. The convention is **layer transform applies first, then internal transform**: `final_model_matrix = internal_base_transform * layer_transform_matrix`. This means the layer transform operates in the model's local space (e.g., `position` shifts the model relative to its own center, not world origin). Layers compute `layer_transform_matrix` from the three fields using the helper pattern: `T(position) * Rx(rotation.x) * Ry(rotation.y) * Rz(rotation.z) * S(scale)`.

For the hummingbird layer, `internal_base_transform` is the existing `base_transform` (the -90 deg Y rotation for side profile). The layer transform wraps around it.

### Motion Blur

The current motion blur implementation (multi-pass accumulation with additive blending and `MOTION_BLUR_SAMPLES`) moves into `layer_hummingbird.c`'s `draw()` method. The sub-frame loop is layer-internal: it clears depth (but not color) between sub-frames, sets `GL_ONE, GL_ONE` additive blending, and restores standard blending before returning. This is an internal detail of the hummingbird layer — other layers are not required to support motion blur. Currently disabled (`MOTION_BLUR_SAMPLES = 1`).

### Signature change

`renderer_frame` no longer takes a `Model *`. New signature:

```c
void renderer_frame(const AudioBands *bands, float dt);
```

## Key Handling (in `main.c`)

```c
case SDL_KEYDOWN: {
    SDL_Keycode sym = event.key.keysym.sym;
    int idx = -1;
    if (sym >= SDLK_1 && sym <= SDLK_9)
        idx = sym - SDLK_1;        // keys 1-9 -> indices 0-8
    else if (sym == SDLK_0)
        idx = 9;                     // key 0 -> index 9
    if (idx >= 0 && idx < s_layer_count) {
        Layer *l = s_layers[idx];
        if (l->current_option < l->option_count)
            l->current_option++;
        else
            l->current_option = 0;
    }
    break;
}
```

Layers don't know about input. `main.c` owns key-to-layer mapping.

## Hummingbird Layer (`src/layer_hummingbird.c/.h`)

### User data

```c
typedef struct {
    Model *model;

    // Scene shader
    GLuint scene_prog;
    ModelUniforms scene_uniforms;

    // Bloom pipeline (option 2 only)
    GLuint extract_prog, blur_prog, composite_prog;
    GLuint fbo_pre_bloom, tex_pre_bloom, rbo_depth;  // layer's private FBO (NOT the shared scene FBO)
    GLuint fbo_bloom[2], tex_bloom[2];
    int fb_width, fb_height, bloom_width, bloom_height;
    GLuint quad_vbo;
} HummingbirdData;
```

### Options

| Option | Behavior |
|--------|----------|
| 0 | Off (not drawn) |
| 1 | Hummingbird without bloom — draws directly into shared scene FBO |
| 2 | Hummingbird with bloom — renders to own FBO, runs bloom pipeline, composites result into shared scene FBO with additive blending |

### Public API

```c
// layer_hummingbird.h
Layer *layer_hummingbird_create(void);
```

Returns a heap-allocated `Layer` with all vtable pointers set and `HummingbirdData` allocated in `user_data`. Caller passes it to `renderer_add_layer()`.

### What moves from `renderer.c` to `layer_hummingbird.c`

- Scene shader loading and `ModelUniforms` population
- `glBindAttribLocation` calls for scene shader (attribs 0-4) and post-process shaders (attribs 0-1)
- Bloom shader loading (extract, blur, composite programs)
- Bloom FBO creation (`create_scene_fbo`, `create_fbo_color` for bloom)
- The model rendering logic from `renderer_frame` (bind scene prog, set audio uniforms, model_update, model_draw)
- The bloom pipeline logic (extract pass, blur ping-pong, composite with bloom intensity)
- Motion blur sub-frame loop

### What stays in `renderer.c`

- Top-level scene FBO (shared canvas for all layers)
- Composite-to-screen pass (final blit from scene FBO to default framebuffer)
- Fullscreen quad VBO and draw utility
- Layer array management and iteration
- `renderer_init`, `renderer_resize`, `renderer_shutdown` (managing shared resources)

### What stays unchanged

- `model.c` / `model.h` — no changes needed. It's already a clean module.
- `audio.c` / `audio.h` — no changes needed.
- `shader.c` / `shader.h` — no changes needed.

## Init / Teardown Sequence

### Startup (`main.c`)

```
SDL_Init()
renderer_init(fb_w, fb_h)          // creates shared scene FBO, quad VBO, composite shader
audio_init()

Layer *hb = layer_hummingbird_create();
hb->init(hb, fb_w, fb_h);          // loads model, compiles shaders, creates bloom FBOs
hb->current_option = 1;            // start enabled, no bloom
renderer_add_layer(hb);
```

### Shutdown (`main.c`)

```
for each layer:
    layer->shutdown(layer)   // shutdown() frees user_data internally
    free(layer)              // main.c frees the Layer struct itself
renderer_shutdown()
audio_shutdown()
SDL_Quit()
```

## File Structure (new/changed files)

```
CMakeLists.txt               # CHANGED — add layer_hummingbird.c to sources
src/
  layer.h                    # NEW — Layer interface struct
  layer_hummingbird.h        # NEW — Hummingbird layer create function
  layer_hummingbird.c        # NEW — Hummingbird rendering (extracted from renderer.c)
  renderer.h                 # CHANGED — new API: add_layer, get_scene_fbo, draw_fullscreen_quad, get_dimensions; remove Model* from renderer_frame
  renderer.c                 # CHANGED — shed model-specific code, add layer iteration
  main.c                     # CHANGED — layer creation, registration, key handling
  model.h                    # UNCHANGED
  model.c                    # UNCHANGED
  audio.h                    # UNCHANGED
  audio.c                    # UNCHANGED
```

## CLAUDE.md Update

Add a "Layer System" section to CLAUDE.md covering:

1. **Architecture overview**: layers as separate compilation units with a common vtable interface
2. **How to add a new layer**: create `layer_foo.c/.h`, implement vtable, register in `main.c`, add to `CMakeLists.txt`
3. **How to add options to an existing layer**: increment `option_count`, handle new option value in `draw`
4. **GL state contract**: layers must restore blend/depth state before returning from `draw`
5. **Decision prompt**: when new rendering behavior is requested, clarify whether it is:
   - A new layer
   - A new option on an existing layer
   - A replacement for an existing layer or option
   - A modification to the composition/post-processing pipeline (renderer.c)

## Presets (Future, Not Implemented Now)

A preset is a static array of structs:

```c
typedef struct {
    int layer_index;
    int option;
    float position[3];
    float rotation[3];
    float scale[3];
} PresetEntry;

typedef struct {
    const char *name;
    PresetEntry entries[MAX_LAYERS];
    int entry_count;
} Preset;
```

Applying a preset: iterate entries, set each layer's `current_option`, `position`, `rotation`, `scale`. Layers not mentioned in the preset are turned off (`current_option = 0`). A single button cycles through a static array of presets.
