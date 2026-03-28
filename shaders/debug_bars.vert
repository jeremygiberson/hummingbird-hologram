/*
 * debug_bars.vert — Debug bar overlay vertex shader (GLSL ES 100 syntax)
 *
 * Each bar is a quad drawn with two triangles. The vertex positions
 * are computed on the CPU and passed in NDC (normalized device coords).
 */

attribute vec2 a_position;
attribute vec3 a_color;

varying vec3 v_color;

void main() {
    gl_Position = vec4(a_position, 0.0, 1.0);
    v_color = a_color;
}
