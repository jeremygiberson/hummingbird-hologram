/* blur.frag — Bloom pass 2: Gaussian blur (GLSL ES 100 syntax)
 *
 * 9-tap 1D gaussian blur, direction controlled by u_horizontal.
 * Called twice per iteration (H then V) for separable 2D blur.
 * Multiple iterations produce wider, softer bloom. */

uniform sampler2D u_image;
uniform int u_horizontal;     /* 1 = horizontal pass, 0 = vertical */
uniform vec2 u_texel_size;    /* 1.0 / texture dimensions */

varying vec2 v_texcoord;

void main() {
    vec2 offset;
    if (u_horizontal == 1) {
        offset = vec2(u_texel_size.x, 0.0);
    } else {
        offset = vec2(0.0, u_texel_size.y);
    }

    /* Gaussian weights for 9 taps (sigma ~ 4) */
    float w0 = 0.227027;
    float w1 = 0.1945946;
    float w2 = 0.1216216;
    float w3 = 0.054054;
    float w4 = 0.016216;

    /* Center tap */
    vec3 result = texture2D(u_image, v_texcoord).rgb * w0;

    /* Symmetric taps */
    result += texture2D(u_image, v_texcoord + offset * 1.0).rgb * w1;
    result += texture2D(u_image, v_texcoord - offset * 1.0).rgb * w1;
    result += texture2D(u_image, v_texcoord + offset * 2.0).rgb * w2;
    result += texture2D(u_image, v_texcoord - offset * 2.0).rgb * w2;
    result += texture2D(u_image, v_texcoord + offset * 3.0).rgb * w3;
    result += texture2D(u_image, v_texcoord - offset * 3.0).rgb * w3;
    result += texture2D(u_image, v_texcoord + offset * 4.0).rgb * w4;
    result += texture2D(u_image, v_texcoord - offset * 4.0).rgb * w4;

    gl_FragColor = vec4(result, 1.0);
}
