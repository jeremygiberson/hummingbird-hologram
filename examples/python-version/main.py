#!/usr/bin/env python3
"""Hummingbird Hologram — Audio-reactive low-poly visualizer.

Set HOLOGRAM_PLATFORM=pi for Raspberry Pi (OpenGL ES 3.1).
Default is 'desktop' (OpenGL 3.3 on macOS/Linux).
"""

import sys
import time
import pygame
import moderngl

from config import PLATFORM, WINDOW_WIDTH, WINDOW_HEIGHT, TARGET_FPS
from audio import AudioPipeline
from model import load_model
from renderer import Renderer


def create_context():
    """Create a pygame window and ModernGL context appropriate for the platform."""
    pygame.init()
    pygame.display.set_caption("Hummingbird Hologram")

    if PLATFORM == "pi":
        # Request OpenGL ES 3.1 on Raspberry Pi
        pygame.display.gl_set_attribute(pygame.GL_CONTEXT_MAJOR_VERSION, 3)
        pygame.display.gl_set_attribute(pygame.GL_CONTEXT_MINOR_VERSION, 1)
        pygame.display.gl_set_attribute(
            pygame.GL_CONTEXT_PROFILE_MASK, pygame.GL_CONTEXT_PROFILE_ES
        )
    else:
        # Desktop OpenGL 3.3 core profile
        pygame.display.gl_set_attribute(pygame.GL_CONTEXT_MAJOR_VERSION, 3)
        pygame.display.gl_set_attribute(pygame.GL_CONTEXT_MINOR_VERSION, 3)
        pygame.display.gl_set_attribute(
            pygame.GL_CONTEXT_PROFILE_MASK, pygame.GL_CONTEXT_PROFILE_CORE
        )

    flags = pygame.OPENGL | pygame.DOUBLEBUF
    pygame.display.set_mode((WINDOW_WIDTH, WINDOW_HEIGHT), flags)

    ctx = moderngl.create_context()
    print(f"[gl] {ctx.info['GL_RENDERER']}")
    print(f"[gl] GL version: {ctx.info['GL_VERSION']}")

    return ctx


def main():
    ctx = create_context()
    clock = pygame.time.Clock()

    # Audio
    audio = AudioPipeline()
    audio.start()

    # Model
    try:
        groups, skin_data, all_positions = load_model()
    except Exception as e:
        print(f"[model] Failed to load: {e}")
        print("[model] Continuing without model (blank screen)")
        groups = None
        all_positions = None

    # Renderer
    renderer = Renderer(ctx)
    if groups and all_positions is not None:
        renderer.setup_from_model(groups, all_positions)

    running = True
    prev_time = time.perf_counter()

    while running:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False
            elif event.type == pygame.KEYDOWN:
                if event.key == pygame.K_ESCAPE or event.key == pygame.K_q:
                    running = False

        now = time.perf_counter()
        dt = now - prev_time
        prev_time = now

        audio.update()
        renderer.update(dt, audio.bands)
        renderer.draw()

        pygame.display.flip()
        clock.tick(TARGET_FPS)

    # Cleanup
    audio.stop()
    pygame.quit()


if __name__ == "__main__":
    main()
