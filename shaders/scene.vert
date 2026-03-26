/* scene.vert — Model rendering vertex shader (GLSL ES 100 syntax)
 * Supports GPU skeletal skinning with up to 32 joints. */

attribute vec3 a_position;
attribute vec3 a_normal;
attribute vec2 a_texcoord;
attribute vec4 a_joints;   /* Joint indices (as float, cast to int) */
attribute vec4 a_weights;  /* Joint weights */

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_proj;
uniform float u_energy;

/* Skinning */
uniform int u_has_skin;
uniform mat4 u_joints[32];

varying vec3 v_normal;
varying vec2 v_texcoord;
varying vec3 v_world_pos;
varying float v_energy;

void main() {
    vec4 pos = vec4(a_position, 1.0);
    vec3 norm = a_normal;

    if (u_has_skin == 1) {
        /* Skeletal skinning: weighted sum of joint transforms */
        int j0 = int(a_joints.x);
        int j1 = int(a_joints.y);
        int j2 = int(a_joints.z);
        int j3 = int(a_joints.w);

        mat4 skin_matrix =
            a_weights.x * u_joints[j0] +
            a_weights.y * u_joints[j1] +
            a_weights.z * u_joints[j2] +
            a_weights.w * u_joints[j3];

        pos = skin_matrix * pos;
        norm = mat3(skin_matrix) * norm;
    }

    vec4 world_pos = u_model * pos;
    gl_Position = u_proj * u_view * world_pos;

    /* Transform normal to world space (approximation — fine for uniform scale) */
    v_normal = normalize(mat3(u_model) * norm);
    v_texcoord = a_texcoord;
    v_world_pos = world_pos.xyz;
    v_energy = u_energy;
}
