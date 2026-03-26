/*
 * shader.c — Shader compilation utilities
 */
#include "shader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* File I/O helpers                                                    */
/* ------------------------------------------------------------------ */

static char *read_file(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[shader] Cannot open: %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    if (out_len) *out_len = len;
    return buf;
}

static time_t file_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return st.st_mtime;
}

/* ------------------------------------------------------------------ */
/* Shader compilation                                                  */
/* ------------------------------------------------------------------ */

static GLuint compile_shader(GLenum type, const char *preamble, const char *source) {
    GLuint shader = glCreateShader(type);
    const char *sources[2] = { preamble, source };
    glShaderSource(shader, 2, sources, NULL);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "[shader] Compilation error:\n%s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint link_program(GLuint vert, GLuint frag) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vert);
    glAttachShader(program, frag);
    glLinkProgram(program);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "[shader] Link error:\n%s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

GLuint shader_load(const char *vert_path, const char *frag_path) {
    char *vert_src = read_file(vert_path, NULL);
    char *frag_src = read_file(frag_path, NULL);
    if (!vert_src || !frag_src) {
        free(vert_src);
        free(frag_src);
        return 0;
    }

    GLuint vert = compile_shader(GL_VERTEX_SHADER, SHADER_PREAMBLE_VERT, vert_src);
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, SHADER_PREAMBLE_FRAG, frag_src);
    free(vert_src);
    free(frag_src);

    if (!vert || !frag) {
        if (vert) glDeleteShader(vert);
        if (frag) glDeleteShader(frag);
        return 0;
    }

    GLuint program = link_program(vert, frag);

    /* Shaders can be deleted after linking */
    glDeleteShader(vert);
    glDeleteShader(frag);

    if (program) {
        fprintf(stderr, "[shader] Loaded: %s + %s → program %u\n",
                vert_path, frag_path, program);
    }
    return program;
}

/* Track file modification times for hot-reload */
static time_t s_vert_mtime = 0;
static time_t s_frag_mtime = 0;

GLuint shader_reload_if_changed(const char *vert_path, const char *frag_path,
                                 GLuint current_program) {
#ifdef HOLOGRAM_SHADER_HOTRELOAD
    time_t vt = file_mtime(vert_path);
    time_t ft = file_mtime(frag_path);

    if (vt != s_vert_mtime || ft != s_frag_mtime) {
        s_vert_mtime = vt;
        s_frag_mtime = ft;

        GLuint new_prog = shader_load(vert_path, frag_path);
        if (new_prog) {
            if (current_program) glDeleteProgram(current_program);
            fprintf(stderr, "[shader] Hot-reloaded: %s + %s\n", vert_path, frag_path);
            return new_prog;
        }
        /* Reload failed — keep current program */
        fprintf(stderr, "[shader] Hot-reload failed, keeping current program\n");
    }
#else
    (void)vert_path;
    (void)frag_path;
    (void)current_program;
#endif
    return 0;
}
