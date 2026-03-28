# Hologram Hummingbird Music Visualizer

## Project Overview

A DIY Pepper's ghost music visualizer that renders an animated 3D hummingbird inside a bell jar enclosure. The device is a self-contained appliance: USB-C power in, display face-down, angled plexiglass/polycarbonate pane at 45° reflects the rendered image upward into a bell jar, creating a floating holographic illusion.

## Hardware Target

- **Compute:** Raspberry Pi Zero 2 W (BCM2710A1, 512MB RAM, VideoCore IV GPU)
- **Display:** ~5.5" AMOLED panel with HDMI driver board (true pixel-off blacks critical for Pepper's ghost)
- **Audio input:** USB or I2S MEMS microphone for audio-reactive bloom
- **Enclosure:** Custom housing with display face-up, 45° polycarbonate reflector, bell jar on top
- **Power:** USB-C, target < 3W total system draw

## Architecture

Single C binary that boots as the only userspace process on a minimal Buildroot Linux image. No X11, no desktop — direct DRM/KMS + EGL/GLES2 via SDL2's KMSDRM backend.

### Rendering Pipeline

1. Load glTF model (hummingbird, 656 verts / 3714 indices, one 1024×1024 base color texture)
2. Main loop at 30fps:
   - Sample microphone → FFT (kissfft) → extract bass/mid/high energy
   - Animate model (skeletal: sample bone TRS keyframes → compute joint matrices → GPU skinning)
   - Render scene to FBO (hummingbird on pure black background)
   - Bloom pipeline:
     a. Bright extract pass (FBO → brightness threshold)
     b. Multi-pass gaussian blur (horizontal + vertical ping-pong)
     c. Composite: scene + bloom, modulated by audio energy uniforms
   - Blit final composite to screen

### Cross-Platform Strategy

The same codebase compiles on macOS (for development) and ARM Linux (for Pi target):

- **macOS:** SDL2 Cocoa backend, OpenGL 2.1 context, desktop GL headers
- **Pi:** SDL2 KMSDRM backend, EGL + GLES2 context, Mesa VC4 Gallium driver
- **Shaders:** Written in GLSL ES 100 syntax. A platform preamble is prepended at load time to handle `precision` qualifiers (GLES2) vs `#version 120` + `#define` away precision (desktop GL)
- **platform.h:** Thin shim (~30 lines) that selects GL headers and defines shader preambles

## Layer System

Rendering content is organized into independent layers, each a separate compilation unit implementing a common vtable interface defined in `src/layer.h`. This makes it easy to add, remove, or swap visual effects without touching the core rendering pipeline.

### Architecture Overview

Each layer is a `Layer` struct containing:
- **Metadata:** `name`, `option_count` (number of active modes), `current_option` (0 = off, 1..option_count = active mode)
- **Per-layer transform:** `position[3]`, `rotation[3]` (Euler degrees, X→Y→Z), `scale[3]` (default `{1,1,1}`)
- **Vtable:** five function pointers — `init`, `update`, `draw`, `resize`, `shutdown`
- **`user_data`:** opaque pointer for layer-private state (heap-allocated by the layer's init)

`renderer.c` owns the shared scene FBO and orchestrates the frame: it iterates all registered layers (back to front), calling `update` then `draw` on each enabled layer, then composites the scene FBO to the screen. Keys 1–0 toggle individual layers.

### Current Layers

| Key | Layer | option_count | Options |
|-----|-------|-------------|---------|
| 1 | Hummingbird | 2 | 1 = no bloom, 2 = with bloom |

### How to Add a New Layer

1. Create `src/layer_foo.h` and `src/layer_foo.c`.
2. Implement the five vtable functions:
   - `bool foo_init(Layer *self, int fb_width, int fb_height)` — allocate `user_data`, compile shaders, upload geometry. Return false on failure.
   - `void foo_update(Layer *self, const AudioBands *bands, float dt)` — advance animation state, write `self->position/rotation/scale` if needed.
   - `void foo_draw(Layer *self, const AudioBands *bands)` — issue GL draw calls. Must leave the scene FBO bound when done (see GL State Contract below).
   - `void foo_resize(Layer *self, int fb_width, int fb_height)` — recreate any size-dependent FBOs.
   - `void foo_shutdown(Layer *self)` — free shaders, buffers, and `user_data`.
3. Provide a factory function that fills a `Layer` struct and returns a pointer:
   ```c
   Layer *layer_foo_create(void);
   ```
4. In `main.c`, call `renderer_add_layer(layer_foo_create())` before entering the render loop.
5. Add `src/layer_foo.c` to the `target_sources(...)` list in `CMakeLists.txt`.
6. Update the Current Layers table above.

### How to Add Options to an Existing Layer

1. Increment `option_count` in the layer's factory function.
2. In `draw()`, branch on `self->current_option` to select the new behavior.
3. Update the Current Layers table above with the new option description.

### GL State Contract

**Renderer guarantees before calling `draw()`:**
- The shared scene FBO is bound (`renderer_get_scene_fbo()`).
- Depth test and blending are in their default state (depth test on, blending off).
- No shader program is active.

**Layers must restore before returning from `draw()`:**
- Rebind the scene FBO if they bound a different FBO (e.g., for ping-pong bloom buffers).
- Leave the GL blend and depth state as they found it, or explicitly reset it.
- Do not leave a VAO/VBO bound that would confuse subsequent layers.

### Shared Utilities (renderer.h)

Layers may call these helpers from `renderer.h`:

| Function | Purpose |
|----------|---------|
| `renderer_get_scene_fbo()` | Returns the shared scene FBO handle to rebind after off-screen passes |
| `renderer_get_dimensions(int *w, int *h)` | Query current framebuffer size |
| `draw_fullscreen_quad()` | Draw a fullscreen quad (attrib 0 = position vec2, attrib 1 = texcoord vec2); caller binds shader first |
| `create_fbo_color(GLuint *fbo, GLuint *tex, int w, int h)` | Create a color-only FBO (no depth) for ping-pong buffers; caller owns the handles |
| `shader_load(const char *vert_path, const char *frag_path)` | Compile and link a GLSL program with platform preamble prepended |

### Decision Prompt for AI Assistants

When a new rendering behavior is requested, clarify which of these applies before writing code:

- **New layer** — a visually independent effect that can be toggled on/off independently (e.g., a particle system, a background gradient, a second animated model). Create `layer_foo.c`.
- **New option on an existing layer** — a variant of an existing effect (e.g., hummingbird with a different shader, bloom on/off). Increment `option_count` and branch in `draw()`.
- **Replacement** — the old behavior is obsolete. Edit the existing layer in place.
- **Composition pipeline change** — the change affects how layers are blended together (e.g., switching from additive to alpha blending at the compositor level). Edit `renderer_frame()` in `renderer.c`.

## Dependencies

### Runtime Libraries
- **SDL2** — windowing, GL context, audio capture, input events
- **OpenGL ES 2.0** (Pi) / **OpenGL 2.1** (macOS) — rendering
- **kissfft** — single-file FFT for audio analysis (vendored in src/)
- **cgltf** — single-header glTF 2.0 loader (vendored in src/)

### Build Tools
- **CMake** >= 3.16
- **Buildroot** (for Pi image generation)

### macOS Development Setup
```bash
brew install sdl2 cmake
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(sysctl -n hw.logicalcpu)
./hologram
```

### Pi Cross-Compilation (via Buildroot)
```bash
# From buildroot directory:
make BR2_EXTERNAL=../hologram/buildroot-external hologram_defconfig
make
# Output: output/images/sdcard.img
```

## File Structure

```
hologram/
├── CLAUDE.md                          # This file
├── CMakeLists.txt                     # Build system
├── .gitignore
├── src/
│   ├── main.c                         # Entry point, SDL init, main loop
│   ├── platform.h                     # GL include shim, platform detection
│   ├── renderer.h                     # Renderer interface
│   ├── renderer.c                     # GL setup, bloom pipeline, draw calls
│   ├── shader.h                       # Shader compilation utilities
│   ├── shader.c                       # Load, prepend preamble, compile, link
│   ├── audio.h                        # Audio capture + FFT interface
│   ├── audio.c                        # SDL audio callback, kissfft, energy extraction
│   ├── model.h                        # Model loading, skinning, debug geo interface
│   ├── model.c                        # cgltf glTF loader, skeletal animation, GPU skinning
│   ├── stb_image.h                    # Vendored: stb_image (texture decoding)
│   ├── stb_impl.c                     # stb_image implementation unit
│   ├── kiss_fft.h                     # Vendored: kissfft header
│   ├── kiss_fft.c                     # Vendored: kissfft implementation
│   ├── _kiss_fft_guts.h              # Vendored: kissfft internal header
│   ├── kiss_fft_log.h                 # Vendored: kissfft logging header
│   └── cgltf.h                        # Vendored: cgltf single-header library
├── shaders/
│   ├── scene.vert                     # Vertex shader: model rendering + GPU skinning (up to 32 joints)
│   ├── scene.frag                     # Fragment shader: 3-light setup, iridescence, audio-reactive rim
│   ├── fullscreen.vert                # Fullscreen quad vertex shader (bloom passes)
│   ├── bright_extract.frag            # Bloom: extract bright fragments
│   ├── blur.frag                      # Bloom: gaussian blur (parameterized H/V)
│   └── composite.frag                 # Bloom: final composite + audio modulation
├── assets/
│   └── hummingbird.glb                # Sketchfab-origin hummingbird model (see "GLB Model Structure" below)
├── buildroot-external/
│   ├── Config.in                      # Buildroot external config
│   ├── external.mk                    # Buildroot external makefile
│   ├── external.desc                  # Buildroot external description
│   ├── configs/
│   │   └── hologram_defconfig         # Buildroot defconfig for Pi Zero 2W
│   └── package/hologram/
│       ├── Config.in                  # Package config
│       └── hologram.mk               # Package build rules
├── scripts/
│   ├── S99hologram                    # Buildroot init script
│   └── screenshot.sh                  # Dev tool: launch app, capture window screenshot, kill app
└── .gitignore
```

## Asset Pipeline

1. Open hummingbird .blend in Blender
2. File → Export → glTF 2.0 (.glb)
   - Format: GLB (binary)
   - Include: Mesh, Materials, Textures, Animations
   - Apply Modifiers: Yes
   - Animation: Bake All Actions
3. Place resulting .glb in `assets/hummingbird.glb`

Expected output: ~1.5MB .glb containing 2k tris, two 1024² textures, and animation keyframes.

## GLB Model Structure (hummingbird.glb)

The current asset is a Sketchfab-origin model. Understanding its structure is essential for any work involving transforms, animation, texturing, or orientation.

### Scene Graph (Node Hierarchy)

```
Sketchfab_model          [ROOT, has_matrix — coordinate system conversion]
  └─ root
      └─ GLTF_SceneRootNode   [has_matrix — additional transform]
          └─ Armature_14
              └─ GLTF_created_0
                  ├─ GLTF_created_0_rootJoint   [skeleton root]
                  │   └─ Bone_12                 [root bone, body]
                  │       ├─ Bone.001_6          [neck]
                  │       │   └─ Bone.002_5      [head/beak]
                  │       ├─ Bone.003_7          [tail]
                  │       ├─ Wing1.R_9           [right wing inner]
                  │       │   └─ Wing2.R_8       [right wing outer]
                  │       └─ Wing1.L_11          [left wing inner]
                  │           └─ Wing2.L_10      [left wing outer]
                  ├─ Kolibri_13
                  ├─ Object_7                    [mesh node — first mesh]
                  ├─ Object_8                    [mesh node]
                  └─ Object_9                    [mesh node]
```

**Important:** `Sketchfab_model` and `GLTF_SceneRootNode` both have `has_matrix` set — these contain coordinate system conversion transforms (Blender Z-up → glTF Y-up). Code must walk the full parent chain from mesh node to root and multiply these matrices, or the model will appear upside down or wrongly oriented.

### Mesh Data (meshes[0] = "Object_0")

- **656 vertices**, **3714 indices** (GL_UNSIGNED_SHORT)
- **Vertex attributes:**
  - `POSITION` (vec3) — 656 vertices, extent ~4.88 units diagonal
  - `NORMAL` (vec3)
  - `TEXCOORD_0` (vec2)
  - `TANGENT` (vec4)
  - `JOINTS_0` (uvec4, stored as `cgltf_component_type_r_16u` / unsigned short) — must be converted to float for the vertex shader
  - `WEIGHTS_0` (vec4)

### Skinning

- **1 skin**, 9 joints, skeleton root = `GLTF_created_0_rootJoint`
- Joint order: `[rootJoint, Bone_12, Bone.001_6, Bone.002_5, Bone.003_7, Wing1.R_9, Wing2.R_8, Wing1.L_11, Wing2.L_10]`
- Inverse bind matrices are provided in the skin accessor
- The vertex shader computes `skinMatrix = Σ(weight[i] * jointMatrix[joint[i]])` for up to 4 influences per vertex
- Joint matrices are computed per-frame as `worldTransform(joint) * inverseBindMatrix(joint)`

### Animation

- **1 animation** ("ArmatureAction.002"), duration **19.12s**, **18 channels**, **18 samplers**
- **This is skeletal animation** — channels target bone nodes, NOT the mesh node
- Animation is applied by writing sampled TRS values directly into `cgltf_node` properties (translation/rotation/scale), then recomputing world transforms for the joint hierarchy
- Channel breakdown:
  - Bone_12 (body): translation only (460 keys) — subtle body sway
  - Bone.001_6 (neck): translation only (454 keys)
  - Bone.002_5 (head): translation only (450 keys)
  - Bone.003_7 (tail): translation (451) + rotation (22) + scale (22)
  - Wing1.R/L (inner wings): translation (460) + rotation (431) + scale (431) — main wing flap
  - Wing2.R/L (outer wings): translation (458-459) + rotation (431) + scale (382-431) — secondary wing articulation
- Most channels have ~430-460 keyframes (dense baked animation), tail rotation/scale have only 22 (sparse)

#### Animation Loop Point (Critical)

The baked animation has a **~1.5s "rest" tail** at the end (17.65s–19.12s) where the wing rotation locks to its final keyframe value and stops cycling. Using the full 19.12s duration causes a visible freeze before the loop restarts. The code auto-detects the usable loop point by scanning the Wing1.R rotation channel for the last time the quaternion returns close to its t=0 value (dot product > 0.999). This gives a clean loop at **~17.65s**.

#### Wing Flap Speed

The baked wing flap cycle is ~1.5s per flap (~0.67 flaps/sec) — far too slow for a hummingbird. Wing bones use a separate time accumulator (`anim_time_wings`) running at `WING_ANIM_SPEED` (8x), giving ~5 flaps/sec. Body/head/tail bones use `anim_time` at 1x speed. The `is_wing_node()` helper matches any bone with "Wing" in its name.

### Material & Textures

- **1 material**, PBR metallic-roughness workflow
- `base_color_factor = [1, 1, 1, 1]` (white — no tint, texture provides all color)
- `metallic_factor = 0.0`, `roughness_factor = 0.5`
- `emissive_factor = [0, 0, 0]`
- **GLB contains 2 embedded images** (both 1024×1024 PNG):
  - Image 0 → Texture 0 → material base_color_texture: **vibrant UV-mapped color map** (deep pinks, teals, purples on black background). This has good color — it is NOT pale despite initial impressions.
  - Image 1 → Texture 1 → material normal_texture: tangent-space normal map (not yet wired to the shader)
- **No** metallic-roughness texture, **no** emissive texture
- The material maps textures correctly: mesh 0, primitive 0 uses texture 0 as base color, texture 1 as normal map. Loading images by array order happens to work, but loading by material reference is more robust.

#### Texture Pitfalls (Hard-Won Knowledge)

1. **The embedded GLB texture has good colors.** Earlier investigation incorrectly concluded it was "pale/washed" — the actual cause of washed rendering was the `composite.frag` shader applying gamma correction (`pow(color, 1/2.2)`) and Reinhard tonemapping on every frame, which crushed all color depth. With those removed, the raw texture renders with vibrant colors matching the extracted PNG.

2. **The external `BakedWithColor.png` from the Sketchfab download has DIFFERENT UV layout than the GLB mesh.** Sketchfab's export pipeline remaps UVs. Pixel-level comparison shows zero overlap between the two textures' content regions. The external texture cannot be used as a drop-in replacement despite looking correct in a preview. Always use the GLB's embedded textures.

3. **The external `BakedWithColor.png` is mostly transparent** — the bird content occupies a small region of the 1024×1024 canvas. Sampling pixels at predictable locations (center, corners) will hit transparent black, which can falsely suggest the texture is empty.

4. **The embedded texture has 3 channels (RGB, no alpha).** stb_image with `desired_channels=4` creates alpha=255 everywhere. The non-bird regions are pure black (0,0,0), which is correct for the Pepper's ghost black background.

5. **Average non-black pixel brightness is (79, 144, 153)** — medium teal. Only ~1.6% of non-black pixels are near-white (>240). Bright spots on the rendered chest are genuinely in the texture, not artifacts.

#### Post-Processing Color Pitfalls

The `composite.frag` shader previously applied gamma correction and Reinhard tonemapping that destroyed the texture's color fidelity:
- `pow(color, vec3(1.0/2.2))` — gamma lift that washes out everything. **Removed.** The textures are already in sRGB and the framebuffer is sRGB, so no gamma conversion is needed.
- `color / (color + 1.0)` — Reinhard tonemapping that compresses dynamic range. **Removed.** Replaced with a simple hue-preserving soft clamp (`if (max > 1.0) color /= max`).
- Bloom with threshold=0 extracts the ENTIRE scene and adds it back, effectively doubling brightness. Always use a meaningful threshold (0.7+).

### Orientation & Camera

- The model's native forward direction is -Z (beak points toward -Z in local space)
- A -90° Y rotation is applied to `base_transform` so the bird shows a side profile (beak pointing right)
- Camera distance is auto-computed from the position accessor's `min`/`max` bounding box
- The model's center Y is offset ~-0.34 units; the view matrix compensates

## Key Design Decisions

- **SDL2 over raw DRM/EGL:** Tiny abstraction overhead, massive dev-time savings. SDL2's KMSDRM backend is production-quality and used in RetroArch, Kodi, etc.
- **Vendored single-header libs:** kissfft and cgltf have zero dependencies and compile in milliseconds. No package management needed.
- **GLES2 baseline:** VC4 supports GLES2 fully. GLES3 is not reliably supported on Pi Zero 2W. All bloom effects are achievable with GLES2 FBOs.
- **30fps target:** Leaves GPU headroom for bloom passes. Pi Zero 2W can handle this scene at 60fps but 30 is fine for the visual effect and saves power.
- **Pure black background:** Essential for Pepper's ghost — any non-black pixel becomes visible light on the reflector pane.

## Audio-Reactive Parameters

The FFT output is binned into frequency bands that map to shader uniforms:

| Uniform | Frequency Range | Visual Effect |
|---------|----------------|---------------|
| `u_bass` | 20–250 Hz | Bloom intensity, overall glow pulse |
| `u_mid` | 250–2000 Hz | Color temperature shift (warm ↔ cool) |
| `u_high` | 2000–16000 Hz | Bloom radius / sparkle sharpness |
| `u_energy` | Full spectrum RMS | Model scale pulse (subtle breathing) |

All values are smoothed with exponential moving average to avoid jitter.

## Boot Sequence (Pi Target)

1. Kernel boot (~2s with minimal Buildroot config)
2. BusyBox init runs `/etc/init.d/S99hologram`
3. Script sets `SDL_VIDEODRIVER=kmsdrm`, `SDL_AUDIODRIVER=alsa`, execs `/usr/bin/hologram`
4. Hologram binary: SDL init → load assets → enter render loop
5. Total power-on to rendering: ~3-5 seconds

## Development Notes

- Suppress macOS GL deprecation warnings with `-DGL_SILENCE_DEPRECATION` (handled automatically by CMakeLists.txt)
- On macOS, the SDL window opens at the display resolution. Use `--width` / `--height` args or let it default to 800×480 to match the Pi display.
- Shader hot-reload: in debug builds, shaders are loaded from disk each frame if modified (check mtime). In release, they could be embedded as string literals via CMake.
- The model animation loop point should be seamless. Verify in Blender that frame 0 and the last frame match poses.
- When no `hummingbird.glb` is present, the app falls back to a spinning debug icosahedron — useful for testing the full bloom and audio pipeline immediately.
- On desktop Linux, `GL_GLEXT_PROTOTYPES` is required for GL 2.0+ function declarations (already handled in platform.h).
- **Screenshot tool:** `./scripts/screenshot.sh [output.png]` launches the app, waits 3s for rendering, captures the window via Swift/CGWindowList, and kills the app. Requires macOS. Default output: `/tmp/hologram_screenshot.png`. Useful for validating visual changes without manual interaction.
- **Vertex attribute layout** (must match between renderer.c `glBindAttribLocation` and model.c draw): 0=position, 1=normal, 2=texcoord, 3=joints, 4=weights. Post-process shaders use: 0=position, 1=texcoord.
- **Skinning on VC4:** The vertex shader uses dynamic array indexing (`u_joints[int(a_joints.x)]`) which works on desktop GL 2.1 but may not work on VC4's GLSL ES 100. If it doesn't, the skinning will need to be computed on the CPU or use an if/else chain instead of dynamic indexing.
- **Motion blur** is implemented as multi-pass accumulation in `renderer_frame`: render N sub-frames at `dt/N` time offsets with `GL_ONE, GL_ONE` additive blending and `u_alpha = 1/N` in the fragment shader. Depth is cleared between sub-frames but color is not. Wing bones move fast (8x) so they blur naturally; body barely moves so it stays sharp. Controlled by `MOTION_BLUR_SAMPLES` (1 = disabled, 5 = good quality). Currently disabled.
- **Bloom is currently disabled** in `renderer_frame` (bloom intensity set to 0, extract/blur passes skipped). The pipeline (FBOs, shaders) is intact and can be re-enabled by restoring the bloom passes in `renderer_frame`. When re-enabling, be aware that `composite.frag` no longer applies gamma or tonemapping — just `scene + bloom * intensity` with a hue-preserving clamp.

## GLSL ES 100 Constraints (VC4 Compatibility)

These are restrictions enforced by the Pi Zero 2W's VideoCore IV GPU. All shaders must comply:

- No `const float[]` array initializer syntax (GLSL 3.30+). Use individual variables.
- No `for` loops indexing into arrays with non-constant expressions on some drivers. Prefer unrolled taps.
- `precision mediump float;` is required in fragment shaders (handled by the preamble system).
- No `layout()` qualifiers. Use `glBindAttribLocation()` from C code instead.
- `texture2D()` only, not `texture()` (which is GLSL 1.30+).
- Integer uniforms (`uniform int`) work but integer arithmetic in shaders can be slow. Prefer float math.

## TODO

- [x] Export hummingbird.glb from Blender and place in assets/
- [x] Wire up cgltf animation keyframe sampling in model_update() — implemented as full skeletal animation with GPU skinning
- [x] Fix model orientation, camera framing, and texture mapping
- [x] Fix composite.frag gamma/tonemapping that was washing out colors
- [x] Implement motion blur (accumulation-based, currently disabled)
- [x] Fix animation loop point (auto-detect last clean wing cycle, cut rest tail)
- [x] Separate wing animation speed (8x) from body animation speed (1x)
- [ ] Add scene lighting to fragment shader (currently texture-only, no lighting)
- [ ] Load and use the normal map texture (image 1 is loaded to slot 1, not yet wired to shader)
- [ ] Load all 3 mesh nodes (Object_7, Object_8, Object_9) — currently only meshes[0] is rendered; the other two may be eyes or separate body parts
- [ ] Re-enable and tune bloom (pipeline intact but disabled; be careful with threshold — 0 extracts everything and doubles brightness)
- [ ] Re-enable and tune motion blur (set MOTION_BLUR_SAMPLES to 3-5)
- [ ] Tune FFT frequency bin boundaries for the mic you end up using
- [ ] Tune EMA smoothing factor (currently 0.15 — lower = smoother, higher = more responsive)
- [ ] Test on actual Pi Zero 2W hardware
- [ ] Verify SDL2 KMSDRM backend works with chosen display (HDMI vs DSI)
- [ ] Verify dynamic array indexing in skinning shader works on VC4 (may need CPU skinning fallback)
- [ ] Build Buildroot image and test boot-to-render time
- [ ] Design and fabricate enclosure
- [ ] Test polycarbonate vs glass reflector at this scale
- [ ] Measure total power draw and verify USB-C budget
- [ ] Consider adding --flip-y flag for Pepper's ghost mirror inversion if needed
