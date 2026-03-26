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
    if (!s_active) return;

    /* Copy samples from ring buffer into FFT input */
    SDL_LockMutex(s_ring_mutex);
    int available = (s_ring_write - s_ring_read + RING_SIZE) % RING_SIZE;
    if (available < FFT_SIZE) {
        SDL_UnlockMutex(s_ring_mutex);
        return;  /* Not enough samples yet */
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

    /* Extract band energies */
    float raw_bass   = band_energy(BASS_LO, BASS_HI);
    float raw_mid    = band_energy(MID_LO, MID_HI);
    float raw_high   = band_energy(HIGH_LO, HIGH_HI);
    float raw_energy = band_energy(0, FFT_SIZE / 2);

    /* Exponential moving average smoothing */
    s_bands.bass   = lerp(s_bands.bass,   raw_bass,   SMOOTHING);
    s_bands.mid    = lerp(s_bands.mid,    raw_mid,    SMOOTHING);
    s_bands.high   = lerp(s_bands.high,   raw_high,   SMOOTHING);
    s_bands.energy = lerp(s_bands.energy, raw_energy, SMOOTHING);
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
