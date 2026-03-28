/*
 * particles.frag — Particles Dance effect (GLSL ES 100 syntax)
 *
 * Adapted from Shadertoy "Particles Dance" by unknown author.
 * https://www.shadertoy.com/view/MdfBz7
 *
 * Audio FFT texture lookups replaced with uniform-driven synthesis.
 * Color varies per-particle based on its frequency band assignment.
 */

#define M_PI 3.1415926535897932384626433832795

uniform vec2 u_resolution;
uniform float u_time;
uniform float u_bass;
uniform float u_mid;
uniform float u_high;
uniform float u_energy;

varying vec2 v_texcoord;

float random(vec2 co)
{
    float a = 12.9898;
    float b = 78.233;
    float c = 43758.5453;
    float dt = dot(co, vec2(a, b));
    float sn = mod(dt, 3.14);
    return fract(sin(sn) * c);
}

/* HSV to RGB conversion */
vec3 hsv2rgb(vec3 c)
{
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main()
{
    vec4 outColor = vec4(0.0);
    float time = u_time * 0.1;

    /* Normalized UV centered at origin, aspect-corrected.
     * Scale by 1.3 so the UV "canvas" extends 30% beyond the
     * window edges. */
    vec2 uvNorm = v_texcoord;
    vec2 uv = -0.5 + uvNorm;
    uv /= vec2(u_resolution.y / u_resolution.x, 1.0);
    uv *= 1.3;

    for (float i = 0.0; i < 300.0; i += 1.0) {
        float f1 = mod(i * 0.101213, 0.28);

        /* Map f1 (0..0.28) to a blend of bass/mid/high */
        float fft1 = mix(u_bass, mix(u_mid, u_high, smoothstep(0.14, 0.28, f1)),
                         smoothstep(0.0, 0.14, f1));
        /* Add some variation per particle */
        fft1 = clamp(fft1 + 0.05 * sin(i * 0.7 + time * 3.0), 0.0, 1.0);

        /* Log-scale orbit radius */
        float fft_log = log(1.0 + fft1 * 4.0) / log(5.0);
        float r = fft_log * 1.0;
        float r1 = (fft_log / 4.0) * random(vec2(uv));
        float a = random(vec2(i, i)) * (M_PI * 2.0);

        vec2 center = vec2(cos(a), sin(a)) * r;
        vec2 center2 = vec2(cos(a), sin(a)) * r1;

        float dist = length(uv - center);
        float dist2 = length(uv - center - center2);

        float brightness = 1.0 / pow(0.001 + dist * 350.0, 2.0);
        float brightness2 = 1.0 / pow(0.001 + dist2 * 500.0, 2.0);

        /* Per-particle color based on frequency band */
        float band_t = smoothstep(0.0, 0.28, f1);
        float hue_base = mix(0.0, 0.75, band_t);
        float hue_vary = random(vec2(i, i + 1.0)) * 0.12 - 0.06;
        float hue = fract(hue_base + hue_vary + time * 0.3);
        float sat = 0.7 + 0.3 * fft1;
        float val = 1.0;
        vec3 color = hsv2rgb(vec3(hue, sat, val));

        vec3 col = color * brightness2 * fft1 * 2.0;
        col += color * brightness * fft1 * 1.5;

        outColor.rgb += col;
    }

    /* Grid overlay */
    float grid = smoothstep(
        sin(length(uv.y - 0.5) * (800.0 * length(uv.y + 0.5))) *
        sin(length(uv.x + 0.5) * (800.0 * length(uv.x - 0.5))),
        0.0, 1.0);
    outColor.rgb += outColor.rgb * vec3(grid) * 0.6;

    gl_FragColor = outColor;
}
