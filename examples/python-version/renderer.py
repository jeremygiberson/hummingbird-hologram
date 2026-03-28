"""Renderer — sets up ModernGL context, manages shaders, camera, and draws the model."""

import math
import numpy as np
import moderngl
import pyrr

from config import (
    PLATFORM, WINDOW_WIDTH, WINDOW_HEIGHT,
    CAMERA_ORBIT_SPEED, CAMERA_ORBIT_RADIUS,
    CAMERA_BOB_AMPLITUDE, CAMERA_BOB_SPEED,
    FLAP_ANGLE_MIN, FLAP_ANGLE_MAX, FLAP_FREQUENCY,
)

# Shader sources
VERTEX_SHADER = """
#version 330

in vec3 in_position;
in vec3 in_normal;
in vec2 in_texcoord;
in vec3 in_color;

uniform mat4 u_mvp;
uniform mat4 u_model;

out vec3 v_normal;
out vec2 v_texcoord;
out vec3 v_color;

void main() {
    gl_Position = u_mvp * vec4(in_position, 1.0);
    v_normal = mat3(u_model) * in_normal;
    v_texcoord = in_texcoord;
    v_color = in_color;
}
"""

FRAGMENT_SHADER = """
#version 330

in vec3 v_normal;
in vec2 v_texcoord;
in vec3 v_color;

out vec4 fragColor;

void main() {
    // Simple directional light
    vec3 light_dir = normalize(vec3(0.5, 1.0, 0.3));
    vec3 n = normalize(v_normal);
    float ndl = max(dot(n, light_dir), 0.0);
    float ambient = 0.3;
    float lighting = ambient + (1.0 - ambient) * ndl;

    fragColor = vec4(v_color * lighting, 1.0);
}
"""

# GLES 3.1 variants for Pi
VERTEX_SHADER_ES = """
#version 310 es
precision highp float;

in vec3 in_position;
in vec3 in_normal;
in vec2 in_texcoord;
in vec3 in_color;

uniform mat4 u_mvp;
uniform mat4 u_model;

out vec3 v_normal;
out vec2 v_texcoord;
out vec3 v_color;

void main() {
    gl_Position = u_mvp * vec4(in_position, 1.0);
    v_normal = mat3(u_model) * in_normal;
    v_texcoord = in_texcoord;
    v_color = in_color;
}
"""

FRAGMENT_SHADER_ES = """
#version 310 es
precision highp float;

in vec3 v_normal;
in vec2 v_texcoord;
in vec3 v_color;

out vec4 fragColor;

void main() {
    vec3 light_dir = normalize(vec3(0.5, 1.0, 0.3));
    vec3 n = normalize(v_normal);
    float ndl = max(dot(n, light_dir), 0.0);
    float ambient = 0.3;
    float lighting = ambient + (1.0 - ambient) * ndl;

    fragColor = vec4(v_color * lighting, 1.0);
}
"""


class Camera:
    """Orbital camera that circles the model."""

    def __init__(self):
        self.angle = 0.0
        self.radius = CAMERA_ORBIT_RADIUS
        self.bob_phase = 0.0
        self.target = np.array([0.0, 0.0, 0.0], dtype=np.float32)

    def update(self, dt):
        self.angle += CAMERA_ORBIT_SPEED * dt
        self.bob_phase += CAMERA_BOB_SPEED * dt * 2.0 * math.pi

    def get_view_matrix(self):
        x = self.radius * math.cos(self.angle)
        z = self.radius * math.sin(self.angle)
        y = CAMERA_BOB_AMPLITUDE * math.sin(self.bob_phase)

        eye = np.array([x, y, z], dtype=np.float32)
        return pyrr.matrix44.create_look_at(
            eye, self.target, np.array([0.0, 1.0, 0.0], dtype=np.float32),
            dtype=np.float32,
        )


class FlapAnimator:
    """Drives wing flapping via a sine wave."""

    def __init__(self):
        self.phase = 0.0
        self.frequency = FLAP_FREQUENCY

    def update(self, dt, audio_amplitude=0.0):
        # Audio drives flap speed: quiet = base freq, loud = 3x base
        speed_mult = 1.0 + audio_amplitude * 2.0
        self.phase += self.frequency * speed_mult * dt * 2.0 * math.pi

    def get_angle_degrees(self):
        """Return current wing rotation angle in degrees."""
        t = math.sin(self.phase)  # -1 to 1
        return FLAP_ANGLE_MIN + (FLAP_ANGLE_MAX - FLAP_ANGLE_MIN) * (t * 0.5 + 0.5)


class Renderer:
    """Manages the ModernGL context, shaders, and draw calls."""

    def __init__(self, ctx):
        self.ctx = ctx
        self.camera = Camera()
        self.flap = FlapAnimator()
        self.prog = None
        self.vaos = {}       # name -> VAO
        self.vbos = {}       # name -> (vbo, ibo)
        self.model_center = np.array([0.0, 0.0, 0.0], dtype=np.float32)
        self._projection = None
        self._setup_shaders()
        self._update_projection()

    def _setup_shaders(self):
        if PLATFORM == "pi":
            self.prog = self.ctx.program(
                vertex_shader=VERTEX_SHADER_ES,
                fragment_shader=FRAGMENT_SHADER_ES,
            )
        else:
            self.prog = self.ctx.program(
                vertex_shader=VERTEX_SHADER,
                fragment_shader=FRAGMENT_SHADER,
            )

    def _update_projection(self):
        aspect = WINDOW_WIDTH / WINDOW_HEIGHT
        self._projection = pyrr.matrix44.create_perspective_projection(
            fovy=45.0, aspect=aspect, near=0.1, far=100.0, dtype=np.float32,
        )

    def upload_mesh_group(self, name, positions, normals, texcoords, indices, colors):
        """Upload a mesh group to the GPU.

        Args:
            name: group name ('body', 'left_wing', 'right_wing')
            positions: (N, 3) float32
            normals: (N, 3) float32
            texcoords: (N, 2) float32
            indices: (M,) uint32
            colors: (N, 3) float32 per-vertex colors
        """
        # Interleave: position(3) + normal(3) + texcoord(2) + color(3) = 11 floats
        vertex_data = np.hstack([positions, normals, texcoords, colors]).astype(np.float32)

        vbo = self.ctx.buffer(vertex_data.tobytes())
        ibo = self.ctx.buffer(indices.astype(np.uint32).tobytes())

        vao = self.ctx.vertex_array(
            self.prog,
            [(vbo, "3f 3f 2f 3f", "in_position", "in_normal", "in_texcoord", "in_color")],
            index_buffer=ibo,
            index_element_size=4,
        )

        self.vaos[name] = vao
        self.vbos[name] = (vbo, ibo)

    def setup_from_model(self, groups, all_positions):
        """Upload all mesh groups and compute camera framing."""
        # Compute model center and camera radius from bounding box
        bbox_min = all_positions.min(axis=0)
        bbox_max = all_positions.max(axis=0)
        self.model_center = (bbox_min + bbox_max) / 2.0
        extent = np.linalg.norm(bbox_max - bbox_min)
        self.camera.radius = extent * 1.5
        self.camera.target = self.model_center.copy()

        # Assign colors by group
        group_colors = {
            "body": np.array([0.2, 0.8, 0.6], dtype=np.float32),       # teal
            "left_wing": np.array([0.8, 0.2, 0.6], dtype=np.float32),  # magenta
            "right_wing": np.array([0.8, 0.2, 0.6], dtype=np.float32), # magenta
        }

        for name, group in groups.items():
            # Expand indexed geometry to per-face vertices for flat shading + per-face color
            idx = group.indices
            pos = group.positions[idx]
            nrm = group.normals[idx]
            tex = group.texcoords[idx]

            # Compute face normals for flat shading
            face_normals = np.zeros_like(pos)
            for i in range(0, len(pos), 3):
                v0, v1, v2 = pos[i], pos[i + 1], pos[i + 2]
                fn = np.cross(v1 - v0, v2 - v0)
                ln = np.linalg.norm(fn)
                if ln > 1e-8:
                    fn /= ln
                face_normals[i] = fn
                face_normals[i + 1] = fn
                face_normals[i + 2] = fn

            colors = np.tile(group_colors.get(name, np.array([0.5, 0.5, 0.5])), (len(pos), 1))
            new_indices = np.arange(len(pos), dtype=np.uint32)

            self.upload_mesh_group(name, pos, face_normals, tex, new_indices, colors)

    def update(self, dt, audio_bands=None):
        amplitude = audio_bands.amplitude if audio_bands else 0.0
        self.camera.update(dt)
        self.flap.update(dt, amplitude)

    def draw(self):
        self.ctx.clear(0.0, 0.0, 0.0, 1.0)
        self.ctx.enable(moderngl.DEPTH_TEST)

        view = self.camera.get_view_matrix()
        flap_angle = self.flap.get_angle_degrees()

        for name, vao in self.vaos.items():
            # Build model matrix: wings get flap rotation
            model = pyrr.matrix44.create_identity(dtype=np.float32)

            if "wing" in name:
                # Rotate around X axis at shoulder pivot
                angle_rad = math.radians(flap_angle)
                if "left" in name:
                    angle_rad = -angle_rad  # mirror
                rot = pyrr.matrix44.create_from_x_rotation(angle_rad, dtype=np.float32)

                # Translate to shoulder, rotate, translate back
                # Approximate shoulder position at model center Y, offset X
                shoulder_offset = np.array([0.0, 0.0, 0.0], dtype=np.float32)
                t1 = pyrr.matrix44.create_from_translation(-shoulder_offset, dtype=np.float32)
                t2 = pyrr.matrix44.create_from_translation(shoulder_offset, dtype=np.float32)
                model = t2 @ rot @ t1

            mvp = self._projection @ view @ model

            # pyrr matrices are row-major; OpenGL expects column-major.
            # Transpose before uploading so GLSL's M*v works correctly.
            self.prog["u_mvp"].write(np.ascontiguousarray(mvp.T, dtype=np.float32).tobytes())
            self.prog["u_model"].write(np.ascontiguousarray(model.T, dtype=np.float32).tobytes())

            vao.render(moderngl.TRIANGLES)
