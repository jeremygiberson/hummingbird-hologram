/* composite.frag — Bloom pass 3: final composite (GLSL ES 100 syntax)
 *
 * Combines the original scene with the blurred bloom texture.
 * Bloom intensity is modulated by audio energy. */

uniform sampler2D u_scene;
uniform sampler2D u_bloom;
uniform float u_bloom_intensity;
uniform float u_bass;
uniform float u_mid;

varying vec2 v_texcoord;

void main() {
    vec3 scene = texture2D(u_scene, v_texcoord).rgb;
    vec3 bloom = texture2D(u_bloom, v_texcoord).rgb;

    /* Tint bloom slightly based on mid-frequency energy */
    vec3 bloom_tint = mix(
        vec3(1.0, 1.0, 1.0),           /* Neutral */
        vec3(0.8, 0.9, 1.2),           /* Cool blue tint */
        u_mid * 3.0
    );

    /* Additive bloom composite */
    vec3 color = scene + bloom * bloom_tint * u_bloom_intensity;

    /* Soft clamp to prevent blowout while preserving hue */
    float max_c = max(max(color.r, color.g), color.b);
    if (max_c > 1.0) color /= max_c;

    gl_FragColor = vec4(color, 1.0);
}
