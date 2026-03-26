/*
 * audio.h — Audio capture + FFT analysis
 *
 * Captures audio from the default input device via SDL2,
 * runs FFT via kissfft, and exposes smoothed frequency band
 * energy values for use as shader uniforms.
 */
#pragma once

#include <stdbool.h>

/* Frequency band energy values, updated each frame */
typedef struct {
    float bass;     /* 20–250 Hz: bloom intensity */
    float mid;      /* 250–2000 Hz: color temperature */
    float high;     /* 2000–16000 Hz: bloom sharpness */
    float energy;   /* Full spectrum RMS: scale pulse */
} AudioBands;

/* Initialize audio capture. Returns true on success.
 * If no mic is available, returns true but bands will be zero
 * (graceful degradation — the visualizer still runs). */
bool audio_init(void);

/* Call once per frame to process any accumulated audio samples
 * and update the band energy values. */
void audio_update(void);

/* Get current smoothed band values. */
AudioBands audio_get_bands(void);

/* Shutdown audio capture and free resources. */
void audio_shutdown(void);
