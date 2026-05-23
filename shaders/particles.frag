/*
 * particles.frag — Particles Dance effect (GLSL ES 100 syntax)
 *
 * Rebased from an optimized/simplified port of Shadertoy "Particles Dance"
 * (https://www.shadertoy.com/view/MdfBz7):
 *   - 200 particles (down from 600), brightness compensated *4
 *   - Spectrum pre-sampled into 16 bins instead of per-iteration lookup
 *   - Distance/grid math folded into squared form
 *
 * Local tweaks preserved from the original port:
 *   - 1.3x UV scale so highest-magnitude particles extend slightly off
 *     the visible edges (avoids a clean clipped boundary).
 *   - Subtle per-bin sinoidal baseline so visuals stay alive in silence
 *     without overshadowing real audio response (0.05 << typical band
 *     values of 0.3-0.8 when music is playing).
 *
 * The new optimized shader sampled iChannel0 for the spectrum, which we
 * don't have — bass/mid/high are interpolated across the 16 bins instead.
 */

#define M_PI 3.1415926535897932384626433832795
#define NUM_PARTICLES 200
#define NUM_FFT_BINS 16

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

void main()
{
    /* Aspect-corrected, centered UV. 1.3x scale so the highest-magnitude
     * particles extend slightly past the visible edges. */
    vec2 uv = v_texcoord - 0.5;
    uv /= vec2(u_resolution.y / u_resolution.x, 1.0);
    uv *= 1.3;

    /* Hoisted: constant per pixel across the particle loop. */
    float jitter = random(uv);

    /* Synthesize a 16-bin spectrum from the three audio bands by
     * interpolating bass→mid→high across t in [0,1]. Per-bin sin offset
     * gives subtle baseline activity in silence. */
    float fftBins[NUM_FFT_BINS];
    for (int b = 0; b < NUM_FFT_BINS; b++) {
        float t = float(b) / float(NUM_FFT_BINS - 1);
        float band = mix(u_bass,
                         mix(u_mid, u_high, smoothstep(0.5, 1.0, t)),
                         smoothstep(0.0, 0.5, t));
        band += 0.05 * sin(float(b) * 0.7 + u_time * 3.0);
        fftBins[b] = clamp(band, 0.0, 1.0);
    }

    vec3 outColor = vec3(0.0);

    for (int i = 0; i < NUM_PARTICLES; i++) {
        float fi = float(i);
        int binIdx = int(mod(fi * 1.618, float(NUM_FFT_BINS)));
        float fft1 = fftBins[binIdx];

        float r  = fft1 * 0.5;
        float r1 = fft1 * 0.125 * jitter;
        float a  = random(vec2(fi, fi * 0.37)) * (M_PI * 2.0);

        vec2 dir = vec2(cos(a), sin(a));
        vec2 d1  = uv - dir * r;
        vec2 d2  = d1 - dir * r1;

        /* Squared distance with K² folded in: 350² = 122500, 500² = 250000. */
        float b1 = 1.0 / (0.001 + dot(d1, d1) * 122500.0);
        float b2 = 1.0 / (0.001 + dot(d2, d2) * 250000.0);

        vec3 color = vec3(fft1 - 0.8, 0.3, fft1 - 0.2);
        outColor += color * fft1 * (b2 * 2.0 + b1 * 1.5);
    }

    /* Compensate for fewer particles vs the original 600. */
    outColor *= 4.0;

    /* Grid overlay: length() on a scalar collapses to abs(), and the
     * difference-of-squares (y-0.5)(y+0.5) = y² - 0.25 folds two muls. */
    float gy = abs(uv.y * uv.y - 0.25);
    float gx = abs(uv.x * uv.x - 0.25);
    float grid = clamp(sin(gy * 800.0) * sin(gx * 800.0), 0.0, 1.0);
    outColor += outColor * grid * 0.6;

    gl_FragColor = vec4(outColor, 1.0);
}
