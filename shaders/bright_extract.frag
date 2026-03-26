/* bright_extract.frag — Bloom pass 1: extract bright fragments (GLSL ES 100)
 *
 * Fragments brighter than u_threshold are kept; darker ones become black.
 * The threshold is lowered by bass energy so more of the scene blooms
 * when bass hits. */

uniform sampler2D u_scene;
uniform float u_threshold;

varying vec2 v_texcoord;

void main() {
    vec4 color = texture2D(u_scene, v_texcoord);

    /* Perceived luminance (Rec. 709) */
    float brightness = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));

    if (brightness > u_threshold) {
        gl_FragColor = color;
    } else {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
    }
}
