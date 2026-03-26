/* scene.frag — Model rendering fragment shader (GLSL ES 100 syntax)
 *
 * Multi-light setup with iridescence + emissive glow for Pepper's ghost.
 * Output on pure black background is critical. */

uniform sampler2D u_texture0;
uniform sampler2D u_texture1;
uniform float u_bass;
uniform float u_mid;
uniform float u_high;
uniform float u_energy;

varying vec3 v_normal;
varying vec2 v_texcoord;
varying vec3 v_world_pos;
varying float v_energy;

/* Increase saturation of a color */
vec3 saturate_color(vec3 c, float amount) {
    float lum = dot(c, vec3(0.299, 0.587, 0.114));
    return mix(vec3(lum), c, amount);
}

void main() {
    vec4 base_color = texture2D(u_texture0, v_texcoord);

    if (base_color.a < 0.01) discard;

    /* Boost texture saturation — the base texture is pale, designed for PBR */
    vec3 tex_color = saturate_color(base_color.rgb, 1.8);

    vec3 N = normalize(v_normal);
    vec3 V = normalize(-v_world_pos);
    float NdotV = max(dot(N, V), 0.0);

    /* Three-light setup */
    vec3 light1_dir = normalize(vec3(0.5, 0.8, 0.6));   /* Key */
    vec3 light2_dir = normalize(vec3(-0.4, 0.3, -0.4));  /* Fill */
    vec3 light3_dir = normalize(vec3(0.0, -0.5, 0.8));   /* Back/rim enhancer */

    float ndl1 = max(dot(N, light1_dir), 0.0);
    float ndl2 = max(dot(N, light2_dir), 0.0);
    float ndl3 = max(dot(N, light3_dir), 0.0);

    /* Specular (Blinn-Phong) for key light */
    vec3 half1 = normalize(light1_dir + V);
    float spec1 = pow(max(dot(N, half1), 0.0), 48.0);

    /* Iridescent color shift based on view angle (fake thin-film effect) */
    vec3 irid_color = vec3(
        0.5 + 0.5 * cos(NdotV * 6.28 + 0.0),
        0.5 + 0.5 * cos(NdotV * 6.28 + 2.09),
        0.5 + 0.5 * cos(NdotV * 6.28 + 4.19)
    );

    /* Ambient + diffuse */
    vec3 ambient = tex_color * 0.3;
    vec3 diffuse = tex_color * (ndl1 * 0.5 + ndl2 * 0.2 + ndl3 * 0.1);

    /* Specular with iridescent tint */
    vec3 specular = mix(vec3(0.5), irid_color, 0.6) * spec1 * 0.6;

    /* Rim light for holographic glow */
    float rim = 1.0 - NdotV;
    rim = pow(rim, 2.0);

    /* Audio-reactive rim color */
    vec3 rim_base = mix(
        vec3(0.1, 0.5, 0.9),   /* Cool cyan */
        vec3(0.9, 0.2, 0.5),   /* Warm magenta */
        u_mid * 5.0
    );
    vec3 rim_color = mix(rim_base, irid_color, 0.4);
    vec3 emissive = rim_color * rim * (0.5 + u_bass * 3.0);

    /* Subtle iridescent sheen across the surface */
    vec3 sheen = irid_color * tex_color * 0.15 * (1.0 - NdotV);

    /* Final color */
    vec3 color = ambient + diffuse + specular + emissive + sheen;

    /* Energy pulse */
    color *= (1.0 + v_energy * 0.1);

    gl_FragColor = vec4(color, base_color.a);
}
