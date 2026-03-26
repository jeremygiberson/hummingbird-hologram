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

### Material & Textures

- **1 material**, PBR metallic-roughness workflow
- `base_color_factor = [1, 1, 1, 1]` (white — no tint, texture provides all color)
- `metallic_factor = 0.0`, `roughness_factor = 0.5`
- `emissive_factor = [0, 0, 0]`
- **Textures:**
  - Base color texture: 1024×1024 RGBA (the model's actual color map — relatively pale, designed for PBR env-map rendering)
  - Normal map texture: 1024×1024 (available but not currently used in the shader)
  - **No** metallic-roughness texture, **no** emissive texture
- The vibrant iridescent look from asset preview sites comes from PBR environment map reflections, not the texture itself. The fragment shader compensates with a fake thin-film iridescence effect (view-angle-based color shift) and saturation boost.
- Textures must be loaded by **material reference** (`material->pbr_metallic_roughness.base_color_texture`), not by image array order — the image array order in the GLB may not match material slots.

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
- [ ] Load and use the normal map texture (available in the model, not yet wired to the shader)
- [ ] Load all 3 mesh nodes (Object_7, Object_8, Object_9) — currently only meshes[0] is rendered; the other two may be eyes or separate body parts
- [ ] Tune bloom threshold (currently 0.7, bass-reactive down to ~0.4)
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
