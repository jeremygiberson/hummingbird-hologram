"""Configuration constants for the hummingbird visualizer."""

import os

# Platform: "pi" for Raspberry Pi (OpenGL ES 3.1), "desktop" for macOS/Linux desktop GL
PLATFORM = os.environ.get("HOLOGRAM_PLATFORM", "desktop")

# Window
WINDOW_WIDTH = 800
WINDOW_HEIGHT = 800
TARGET_FPS = 60

# Audio
AUDIO_SAMPLE_RATE = 44100
AUDIO_BLOCK_SIZE = 1024
FFT_SIZE = 2048

# Frequency band boundaries (Hz)
BAND_BASS = (20, 250)
BAND_LOW_MID = (250, 1000)
BAND_HIGH_MID = (1000, 4000)
BAND_HIGH = (4000, 16000)

# Smoothing (exponential moving average alpha)
SMOOTHING_FACTOR = 0.1

# Camera
CAMERA_ORBIT_SPEED = 0.3       # radians per second
CAMERA_ORBIT_RADIUS = 5.0      # units from center
CAMERA_BOB_AMPLITUDE = 0.3     # vertical bob range
CAMERA_BOB_SPEED = 0.5         # bob cycles per second

# Flapping
FLAP_ANGLE_MIN = -30.0         # degrees
FLAP_ANGLE_MAX = 30.0          # degrees
FLAP_FREQUENCY = 2.0           # cycles per second (base, no audio)

# Model
MODEL_PATH = os.path.join(os.path.dirname(__file__), "..", "..", "assets", "hummingbird.glb")
