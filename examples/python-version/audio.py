"""Audio capture and FFT analysis pipeline."""

import numpy as np
import threading

from config import (
    AUDIO_SAMPLE_RATE, AUDIO_BLOCK_SIZE, FFT_SIZE,
    BAND_BASS, BAND_LOW_MID, BAND_HIGH_MID, BAND_HIGH,
    SMOOTHING_FACTOR,
)


class AudioBands:
    """Holds smoothed frequency band energies."""
    __slots__ = ("bass", "low_mid", "high_mid", "high", "amplitude")

    def __init__(self):
        self.bass = 0.0
        self.low_mid = 0.0
        self.high_mid = 0.0
        self.high = 0.0
        self.amplitude = 0.0


class AudioPipeline:
    def __init__(self):
        self.bands = AudioBands()
        self._buffer = np.zeros(FFT_SIZE, dtype=np.float32)
        self._write_pos = 0
        self._lock = threading.Lock()
        self._stream = None
        self._freq_bins = np.fft.rfftfreq(FFT_SIZE, d=1.0 / AUDIO_SAMPLE_RATE)

    def start(self):
        """Start audio capture. Imports sounddevice lazily so the rest of the
        app works even if no audio device is available."""
        try:
            import sounddevice as sd
            self._stream = sd.InputStream(
                samplerate=AUDIO_SAMPLE_RATE,
                blocksize=AUDIO_BLOCK_SIZE,
                channels=1,
                dtype="float32",
                callback=self._audio_callback,
            )
            self._stream.start()
            print("[audio] Capture started")
        except Exception as e:
            print(f"[audio] Could not start capture: {e}")
            print("[audio] Running without audio input")

    def stop(self):
        if self._stream is not None:
            self._stream.stop()
            self._stream.close()
            self._stream = None

    def _audio_callback(self, indata, frames, time_info, status):
        """Called from the audio thread — copy samples into the rolling buffer."""
        mono = indata[:, 0]
        n = len(mono)
        with self._lock:
            space = FFT_SIZE - self._write_pos
            if n <= space:
                self._buffer[self._write_pos:self._write_pos + n] = mono
                self._write_pos += n
            else:
                self._buffer[self._write_pos:] = mono[:space]
                remaining = n - space
                self._buffer[:remaining] = mono[space:]
                self._write_pos = remaining

    def update(self):
        """Run FFT on the current buffer and update smoothed band energies.
        Call once per frame from the main thread."""
        with self._lock:
            snapshot = self._buffer.copy()

        # Apply Hann window and compute FFT magnitudes
        window = np.hanning(FFT_SIZE)
        spectrum = np.abs(np.fft.rfft(snapshot * window))

        # Extract band energies
        raw_bass = self._band_energy(spectrum, BAND_BASS)
        raw_low_mid = self._band_energy(spectrum, BAND_LOW_MID)
        raw_high_mid = self._band_energy(spectrum, BAND_HIGH_MID)
        raw_high = self._band_energy(spectrum, BAND_HIGH)
        raw_amplitude = np.sqrt(np.mean(snapshot ** 2))

        # Exponential smoothing
        a = SMOOTHING_FACTOR
        b = self.bands
        b.bass = b.bass + a * (raw_bass - b.bass)
        b.low_mid = b.low_mid + a * (raw_low_mid - b.low_mid)
        b.high_mid = b.high_mid + a * (raw_high_mid - b.high_mid)
        b.high = b.high + a * (raw_high - b.high)
        b.amplitude = b.amplitude + a * (raw_amplitude - b.amplitude)

    def _band_energy(self, spectrum, band):
        """Average magnitude of FFT bins within the given frequency range."""
        lo, hi = band
        mask = (self._freq_bins >= lo) & (self._freq_bins < hi)
        if not np.any(mask):
            return 0.0
        return float(np.mean(spectrum[mask]))
