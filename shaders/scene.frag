/* scene.frag — Starting from texture-only, no lighting. */

uniform sampler2D u_texture0;
uniform sampler2D u_texture1;
uniform float u_bass;
uniform float u_mid;
uniform float u_high;
uniform float u_energy;
uniform float u_alpha;

varying vec3 v_normal;
varying vec2 v_texcoord;
varying vec3 v_world_pos;
varying float v_energy;

void main() {
    vec4 base_color = texture2D(u_texture0, v_texcoord);
    if (base_color.a < 0.01) discard;

    gl_FragColor = vec4(base_color.rgb * u_alpha, base_color.a * u_alpha);
}
