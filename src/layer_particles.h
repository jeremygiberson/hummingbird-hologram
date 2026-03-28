/*
 * layer_particles.h — Particles Dance rendering layer
 *
 * Fullscreen audio-reactive particle effect based on Shadertoy "Particles Dance"
 * (https://www.shadertoy.com/view/MdfBz7). Particles orbit the center in a
 * circular pattern, driven by audio energy bands.
 *
 * Option 1: Particles dance effect
 */
#pragma once

#include "layer.h"

Layer *layer_particles_create(void);
