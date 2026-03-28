Here's a detailed, ordered task list for a coding agent:

---

## Audio Visualizer — Low-Poly Hummingbird (Raspberry Pi 5)

### Project Setup
1. Initialize a Python project with `ModernGL`, `pygame` (for windowing/input), and `numpy` as core dependencies. Target Python 3.11+ on Pi OS. Add `pyaudio` or `sounddevice` for audio capture.
2. Set up a main loop with pygame + ModernGL context targeting OpenGL ES 3.1, 720p resolution, 60fps vsync.
3. Implement an audio capture pipeline: capture system audio or mic input via PipeWire/ALSA using `sounddevice`, run a rolling FFT (1024 or 2048 samples), and expose frequency band energies (bass, low-mid, high-mid, high) plus overall amplitude as a shared data structure updated each frame.

### Core Scene — Baseline Rendering
4. Load a low-poly hummingbird model from a `.obj` or `.glb` file. Parse vertices, faces, and normals. Store geometry in a ModernGL vertex buffer. The model should have separate mesh groups for body, left wing, and right wing (or tag wing faces by convention).
5. Write a minimal vertex + fragment shader pair: flat shading (no lighting), per-face color via a uniform or vertex attribute, and a model-view-projection matrix uniform. Render the bird centered on screen.
6. Implement a simple orbital camera: the camera orbits the bird on a circular path at a fixed radius. Parameterize radius and orbit speed. Add slight vertical bobbing with a sine wave.
7. Implement the flapping animation: rotate wing mesh groups (or wing-tagged vertices) around a shoulder pivot axis using a sine wave. Parameterize flap angle range and flap frequency (cycles per second).

### Audio-Reactive Features (implement in this order)
8. **Flap speed from energy**: Map the smoothed overall audio amplitude (use exponential smoothing, ~0.1 factor) to the flap frequency parameter. Quiet = slow gentle flap, loud = fast aggressive flap.
9. **Per-face color from spectrum**: Assign each face an index (0 to N). Map that index to a frequency bin. Each frame, update the face color by sampling a color gradient LUT (e.g., deep blue → cyan → magenta → white) at a position determined by that bin's energy. Update the vertex color buffer each frame. Since face count is low (~100-300), this is a cheap buffer update.
10. **Vertex displacement from bass**: Each frame on CPU, offset each vertex position along its normal by `bass_energy * scale_factor`. Use the original vertex positions as a base and write displaced positions to the VBO. Clamp the displacement to avoid the mesh inverting. Smooth the bass energy value to avoid jitter.
11. **Body tilt from mid frequencies**: Apply a rotation to the bird's model matrix — pitch forward/backward proportional to smoothed mid-frequency energy. Keep the range subtle (±10-15 degrees).
12. **Wingtip trail lines**: Store the world-space position of each wingtip (2 points) for the last 20-30 frames in a circular buffer. Render as `GL_LINE_STRIP` with a per-vertex alpha attribute that fades from 1.0 (newest) to 0.0 (oldest). Use additive blending for just these lines.
13. **Face explode on transient detection**: Implement a simple onset/transient detector (compare current frame energy to a running average; if ratio > threshold, trigger). On trigger, for each face, offset its vertices along the face normal by a burst amount. Lerp back to original positions over ~0.5 seconds. Use an easing function (ease-out quad) for the return.
14. **Orbit radius from energy**: Modulate the camera orbit radius — tighten (zoom in) when energy is high, widen (zoom out) when quiet. Smooth heavily to avoid motion sickness.

### Background Elements
15. **Spectrum grid**: Create a flat grid mesh (e.g., 32x32 vertices) positioned behind the bird. Each frame, set the Y (height) of each column of vertices to the corresponding FFT bin magnitude. Use a separate color gradient (darker/subtler than the bird). Render before the bird (no depth test needed, or place far back).
16. **Wireframe overlay**: Render the bird a second time with `GL_LINES` (edge list extracted from face data at load time) at a slightly larger scale (1.01x). Set a uniform alpha that pulses with the beat (smoothed amplitude). Use additive blending.

### Polish & Performance
17. Add a global color palette rotation: slowly rotate the LUT index offset over time (e.g., +0.5 per second) so the bird's colors cycle even during quiet sections.
18. Add smooth transitions: all audio-reactive values should use exponential smoothing or lerping to avoid jarring frame-to-frame jumps. Attack should be fast (~0.2), decay should be slow (~0.05).
19. Profile on Pi 5 hardware. Measure frame times. If over budget: reduce grid resolution first, then reduce trail length, then disable wireframe overlay. Target consistent 16ms frames.
20. Add a config file (JSON or YAML) exposing: audio device, FFT size, smoothing factors, color palette, displacement scale, flap speed range, orbit speed, window resolution. This allows tuning without code changes.

---

**Key constraints to include in the agent prompt:**
- Test platform is osx, target platform is Raspberry Pi 5 8GB running Pi OS, OpenGL ES 3.1
- No post-processing passes (bloom, blur). No dynamic shadows. No transparency except trail lines and wireframe overlay.
- All mesh manipulation (displacement, explode) happens on CPU since vertex count is low. Upload modified VBO each frame.
- Render at 800x800.
- Keep all dependencies pip-installable. No compiled C extensions beyond what numpy/pyaudio already provide.
