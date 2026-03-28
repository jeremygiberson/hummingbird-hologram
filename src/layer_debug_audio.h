/*
 * layer_debug_audio.h — Debug overlay for audio analysis values
 *
 * Renders horizontal bars in the top-left corner showing all AudioBands
 * values in real time. Each bar has a distinct color for identification.
 *
 * Option 1: Show debug bars
 */
#pragma once

#include "layer.h"

Layer *layer_debug_audio_create(void);
