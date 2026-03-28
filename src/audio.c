/*
 * audio.c — Audio capture + FFT analysis
 */
#include "audio.h"
#include "kiss_fft.h"

#include <SDL.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define SAMPLE_RATE     22050
#define FFT_SIZE        1024
#define SMOOTHING       0.15f   /* EMA alpha: lower = smoother */

/* Frequency bin boundaries (in Hz → bin index) */
#define BIN(hz) ((int)((hz) * FFT_SIZE / SAMPLE_RATE))
#define BASS_LO   BIN(20)
#define BASS_HI   BIN(250)
#define MID_LO    BIN(250)
#define MID_HI    BIN(2000)
#define HIGH_LO   BIN(2000)
#define HIGH_HI   BIN(16000)

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static SDL_AudioDeviceID s_device = 0;
static bool              s_active = false;

/* Ring buffer for incoming samples */
#define RING_SIZE (FFT_SIZE * 4)
static float s_ring[RING_SIZE];
static int   s_ring_write = 0;
static int   s_ring_read  = 0;
static SDL_mutex *s_ring_mutex = NULL;

/* FFT state */
static kiss_fft_cfg   s_fft_cfg = NULL;
static kiss_fft_cpx   s_fft_in[FFT_SIZE];
static kiss_fft_cpx   s_fft_out[FFT_SIZE];
static float          s_magnitudes[FFT_SIZE / 2];

/* Previous frame magnitudes for spectral flux */
static float s_prev_magnitudes[FFT_SIZE / 2];

/* Beat detection state */
#define BEAT_HISTORY 30         /* ~1 second of history at 30fps */
static float s_bass_history[BEAT_HISTORY];
static int   s_bass_history_idx = 0;
static float s_beat_decay = 0.0f;

/* Onset detection state */
static float s_flux_history[BEAT_HISTORY];
static int   s_flux_history_idx = 0;
static float s_onset_decay = 0.0f;

/* Breathing baseline state */
static float s_breath_time = 0.0f;

/* Smoothed output */
static AudioBands s_bands = {0};

/* ------------------------------------------------------------------ */
/* SDL audio callback (runs on audio thread)                           */
/* ------------------------------------------------------------------ */

static void audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    int sample_count = len / (int)sizeof(float);
    float *samples = (float *)stream;

    SDL_LockMutex(s_ring_mutex);
    for (int i = 0; i < sample_count; i++) {
        s_ring[s_ring_write] = samples[i];
        s_ring_write = (s_ring_write + 1) % RING_SIZE;
    }
    SDL_UnlockMutex(s_ring_mutex);
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static float band_energy(int lo, int hi) {
    if (lo < 0) lo = 0;
    if (hi > FFT_SIZE / 2) hi = FFT_SIZE / 2;
    if (lo >= hi) return 0.0f;

    float sum = 0.0f;
    for (int i = lo; i < hi; i++) {
        sum += s_magnitudes[i];
    }
    return sum / (float)(hi - lo);
}

static float lerp(float a, float b, float t) {
    return a + t * (b - a);
}

/* Hann window to reduce spectral leakage */
static float hann(int i, int n) {
    return 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(n - 1)));
}

/*
 * Apple-style breathing curve.
 *
 * The classic MacBook sleep indicator used a ~3.5s inhale (slow ease-in),
 * ~2.5s exhale (slightly faster ease-out), and a ~1.5s pause at the bottom
 * before the next breath. Total cycle ~7.5s.
 *
 * We approximate this with a piecewise cubic:
 *   Phase 1 (0.0–0.47): Inhale  — smoothstep ease-in-out, 0→1
 *   Phase 2 (0.47–0.80): Exhale — smoothstep ease-in-out, 1→0
 *   Phase 3 (0.80–1.0):  Pause  — hold at 0
 *
 * Returns 0.0–1.0.
 */
#define BREATH_CYCLE 7.5f

static float breathing(float time) {
    float phase = fmodf(time / BREATH_CYCLE, 1.0f);

    if (phase < 0.47f) {
        /* Inhale: 0→1 with ease-in-out */
        float t = phase / 0.47f;
        return t * t * (3.0f - 2.0f * t);  /* smoothstep */
    } else if (phase < 0.80f) {
        /* Exhale: 1→0 with ease-in-out */
        float t = (phase - 0.47f) / 0.33f;
        float s = t * t * (3.0f - 2.0f * t);
        return 1.0f - s;
    } else {
        /* Pause at bottom */
        return 0.0f;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

bool audio_init(void) {
    s_ring_mutex = SDL_CreateMutex();
    if (!s_ring_mutex) return false;

    /* Allocate FFT */
    s_fft_cfg = kiss_fft_alloc(FFT_SIZE, 0, NULL, NULL);
    if (!s_fft_cfg) {
        fprintf(stderr, "[audio] Failed to allocate FFT\n");
        return false;
    }

    /* Open capture device */
    SDL_AudioSpec want = {0};
    want.freq = SAMPLE_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = 1;
    want.samples = FFT_SIZE / 2;  /* ~23ms buffer at 22050 */
    want.callback = audio_callback;

    SDL_AudioSpec have;
    s_device = SDL_OpenAudioDevice(NULL, 1 /* capture */, &want, &have, 0);
    if (s_device == 0) {
        fprintf(stderr, "[audio] No capture device available: %s\n", SDL_GetError());
        fprintf(stderr, "[audio] Running without audio reactivity\n");
        s_active = false;
        return true;  /* Graceful degradation */
    }

    fprintf(stderr, "[audio] Opened capture: %dHz, %d channels, %d samples\n",
            have.freq, have.channels, have.samples);

    SDL_PauseAudioDevice(s_device, 0);  /* Start capture */
    s_active = true;
    return true;
}

void audio_update(void) {
    /* Always advance breathing clock (1/30s per frame at 30fps) */
    s_breath_time += 1.0f / 30.0f;

    if (!s_active) {
        /* No mic — just apply breathing baseline */
        float breath = breathing(s_breath_time) * 0.008f;
        s_bands.bass   = breath;
        s_bands.mid    = breath * 0.7f;
        s_bands.energy = breath;
        return;
    }

    /* Copy samples from ring buffer into FFT input */
    SDL_LockMutex(s_ring_mutex);
    int available = (s_ring_write - s_ring_read + RING_SIZE) % RING_SIZE;
    if (available < FFT_SIZE) {
        SDL_UnlockMutex(s_ring_mutex);
        /* Not enough samples yet — still apply breathing */
        float breath = breathing(s_breath_time) * 0.008f;
        s_bands.bass   = fminf(s_bands.bass   + breath, 1.0f);
        s_bands.mid    = fminf(s_bands.mid    + breath * 0.7f, 1.0f);
        s_bands.energy = fminf(s_bands.energy + breath, 1.0f);
        return;
    }

    /* Take the most recent FFT_SIZE samples */
    int start = (s_ring_write - FFT_SIZE + RING_SIZE) % RING_SIZE;
    for (int i = 0; i < FFT_SIZE; i++) {
        int idx = (start + i) % RING_SIZE;
        s_fft_in[i].r = s_ring[idx] * hann(i, FFT_SIZE);
        s_fft_in[i].i = 0.0f;
    }
    s_ring_read = s_ring_write;
    SDL_UnlockMutex(s_ring_mutex);

    /* Run FFT */
    kiss_fft(s_fft_cfg, s_fft_in, s_fft_out);

    /* Compute magnitudes */
    for (int i = 0; i < FFT_SIZE / 2; i++) {
        float re = s_fft_out[i].r;
        float im = s_fft_out[i].i;
        s_magnitudes[i] = sqrtf(re * re + im * im) / (float)FFT_SIZE;
    }

    /* Extract band energies (raw FFT magnitudes, typically 0.0001–0.01) */
    float raw_bass   = band_energy(BASS_LO, BASS_HI);
    float raw_mid    = band_energy(MID_LO, MID_HI);
    float raw_high   = band_energy(HIGH_LO, HIGH_HI);
    float raw_energy = band_energy(0, FFT_SIZE / 2);

    /* Scale raw magnitudes to ~0–1 range.
     * Laptop mics produce tiny FFT magnitudes. These gains are empirical —
     * they map typical room-volume music to 0.3–0.8 on the bars.
     * Bass is louder in music so needs less gain than high. */
    raw_bass   *= 200.0f;
    raw_mid    *= 300.0f;
    raw_high   *= 500.0f;
    raw_energy *= 250.0f;

    /* Soft clamp to 0–1 */
    if (raw_bass   > 1.0f) raw_bass   = 1.0f;
    if (raw_mid    > 1.0f) raw_mid    = 1.0f;
    if (raw_high   > 1.0f) raw_high   = 1.0f;
    if (raw_energy > 1.0f) raw_energy = 1.0f;

    /* Exponential moving average smoothing */
    s_bands.bass   = lerp(s_bands.bass,   raw_bass,   SMOOTHING);
    s_bands.mid    = lerp(s_bands.mid,    raw_mid,    SMOOTHING);
    s_bands.high   = lerp(s_bands.high,   raw_high,   SMOOTHING);
    s_bands.energy = lerp(s_bands.energy, raw_energy, SMOOTHING);

    /* --- Beat detection ---
     * Compare current bass energy to recent average. A spike above
     * the mean + threshold triggers a beat impulse that decays. */
    s_bass_history[s_bass_history_idx] = raw_bass;
    s_bass_history_idx = (s_bass_history_idx + 1) % BEAT_HISTORY;

    float bass_avg = 0.0f;
    for (int i = 0; i < BEAT_HISTORY; i++) bass_avg += s_bass_history[i];
    bass_avg /= (float)BEAT_HISTORY;

    float beat_threshold = bass_avg * 1.4f + 0.05f;
    if (raw_bass > beat_threshold) {
        s_beat_decay = 1.0f;
    } else {
        s_beat_decay *= 0.85f;  /* Decay ~6 frames to half */
    }
    s_bands.beat = s_beat_decay;

    /* --- Spectral flux ---
     * Sum of positive magnitude changes across all bins (half-wave rectified).
     * Measures how much the spectrum changed since last frame. */
    int half = FFT_SIZE / 2;
    float flux = 0.0f;
    for (int i = 0; i < half; i++) {
        float diff = s_magnitudes[i] - s_prev_magnitudes[i];
        if (diff > 0.0f) flux += diff;
        s_prev_magnitudes[i] = s_magnitudes[i];
    }
    /* Normalize flux to roughly 0–1 range (empirical scaling).
     * Magnitudes are raw (unscaled), so sum of deltas across 512 bins
     * is still very small — need aggressive scaling. */
    float raw_flux = flux * 2000.0f;
    if (raw_flux > 1.0f) raw_flux = 1.0f;
    s_bands.spectral_flux = lerp(s_bands.spectral_flux, raw_flux, SMOOTHING);

    /* --- Onset detection ---
     * Like beat detection but using spectral flux instead of bass.
     * Catches transients across the full spectrum. */
    s_flux_history[s_flux_history_idx] = raw_flux;
    s_flux_history_idx = (s_flux_history_idx + 1) % BEAT_HISTORY;

    float flux_avg = 0.0f;
    for (int i = 0; i < BEAT_HISTORY; i++) flux_avg += s_flux_history[i];
    flux_avg /= (float)BEAT_HISTORY;

    if (raw_flux > flux_avg * 1.5f + 0.05f) {
        s_onset_decay = 1.0f;
    } else {
        s_onset_decay *= 0.80f;  /* Faster decay than beat */
    }
    s_bands.onset = s_onset_decay;

    /* --- Spectral centroid ---
     * Weighted average of frequency bin indices, normalized to 0–1.
     * Low = dark/warm sound, high = bright/sharp. */
    float weighted_sum = 0.0f;
    float mag_sum = 0.0f;
    for (int i = 1; i < half; i++) {
        weighted_sum += (float)i * s_magnitudes[i];
        mag_sum += s_magnitudes[i];
    }
    float centroid = 0.0f;
    /* Noise gate: require meaningful signal before computing centroid.
     * Without this, mic noise produces random centroid jitter even in silence. */
    float noise_gate = 0.02f;  /* Roughly equivalent to scaled energy > 0.04 */
    if (mag_sum > noise_gate) {
        centroid = weighted_sum / mag_sum / (float)half;  /* Normalize to 0–1 */
    }
    s_bands.spectral_centroid = lerp(s_bands.spectral_centroid, centroid, SMOOTHING);

    /* --- Breathing baseline ---
     * Adds an Apple sleep-LED style pulse so visuals are never fully dead.
     * The breath value is additive: when real audio is present it just adds
     * a gentle undertow; when silent it provides the only animation.
     * We apply it to bass/mid/energy since those drive most visual effects.
     * The breath is subtle (0.15 peak) so it doesn't overwhelm real audio. */
    float breath = breathing(s_breath_time) * 0.008f;
    s_bands.bass   = fminf(s_bands.bass   + breath, 1.0f);
    s_bands.mid    = fminf(s_bands.mid    + breath * 0.7f, 1.0f);
    s_bands.energy = fminf(s_bands.energy + breath, 1.0f);
}

AudioBands audio_get_bands(void) {
    return s_bands;
}

void audio_shutdown(void) {
    if (s_device) {
        SDL_CloseAudioDevice(s_device);
        s_device = 0;
    }
    if (s_ring_mutex) {
        SDL_DestroyMutex(s_ring_mutex);
        s_ring_mutex = NULL;
    }
    if (s_fft_cfg) {
        kiss_fft_free(s_fft_cfg);
        s_fft_cfg = NULL;
    }
    s_active = false;
}
