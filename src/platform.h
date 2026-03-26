/*
 * platform.h — GL include shim and platform detection
 *
 * Bridges the gap between desktop OpenGL 2.1 (macOS/Linux) and
 * OpenGL ES 2.0 (Raspberry Pi). GLES2 and GL 2.1 are nearly
 * identical at the API level; the main differences are:
 *   - Header paths
 *   - Precision qualifiers in shaders (GLES2 requires them)
 *   - A few missing function variants
 */
#pragma once

#include <SDL.h>

/* ------------------------------------------------------------------ */
/* GL headers                                                          */
/* ------------------------------------------------------------------ */
#ifdef PLATFORM_PI
    #include <GLES2/gl2.h>
    #include <GLES2/gl2ext.h>
    #include <EGL/egl.h>
#elif defined(__APPLE__)
    #include <OpenGL/gl.h>
    #include <OpenGL/glext.h>
#else
    #define GL_GLEXT_PROTOTYPES
    #include <GL/gl.h>
    #include <GL/glext.h>
#endif

/* ------------------------------------------------------------------ */
/* Shader preambles                                                    */
/*                                                                     */
/* GLES2 shaders require precision qualifiers. Desktop GL 2.1 does    */
/* not have them. We prepend a preamble at shader load time so the    */
/* actual .vert/.frag files are written in GLES2 syntax.              */
/* ------------------------------------------------------------------ */
#ifdef PLATFORM_PI
    #define SHADER_PREAMBLE_VERT \
        "precision mediump float;\n"
    #define SHADER_PREAMBLE_FRAG \
        "precision mediump float;\n"
#else
    /* Desktop GL 2.1: define away precision qualifiers */
    #define SHADER_PREAMBLE_VERT \
        "#version 120\n" \
        "#define lowp\n" \
        "#define mediump\n" \
        "#define highp\n"
    #define SHADER_PREAMBLE_FRAG \
        "#version 120\n" \
        "#define lowp\n" \
        "#define mediump\n" \
        "#define highp\n"
#endif

/* ------------------------------------------------------------------ */
/* SDL GL context attributes                                           */
/* ------------------------------------------------------------------ */
static inline void platform_set_gl_attributes(void) {
#ifdef PLATFORM_PI
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#endif
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}

/* ------------------------------------------------------------------ */
/* Asset path resolution                                               */
/*                                                                     */
/* On Pi (installed via Buildroot): /usr/share/hologram/               */
/* On desktop (dev): relative to executable, or current directory      */
/* ------------------------------------------------------------------ */
#ifdef PLATFORM_PI
    #define ASSET_BASE_PATH "/usr/share/hologram/"
#else
    #define ASSET_BASE_PATH ""
#endif

#define SHADER_PATH(name) ASSET_BASE_PATH "shaders/" name
#define ASSET_PATH(name)  ASSET_BASE_PATH "assets/" name
