/*
 * debug_bars.frag — Debug bar overlay fragment shader (GLSL ES 100 syntax)
 */

varying vec3 v_color;

void main() {
    gl_FragColor = vec4(v_color, 0.85);
}
