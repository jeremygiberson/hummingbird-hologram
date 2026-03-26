/*
 * shader.h — Shader compilation utilities
 */
#pragma once

#include "platform.h"

/* Compile a shader program from vertex + fragment source files.
 * Automatically prepends the platform-appropriate preamble.
 * Returns GL program ID, or 0 on failure. */
GLuint shader_load(const char *vert_path, const char *frag_path);

/* Reload a shader from disk (for hot-reload in debug builds).
 * Returns new program ID if files changed, or 0 if unchanged/error. */
GLuint shader_reload_if_changed(const char *vert_path, const char *frag_path,
                                 GLuint current_program);
