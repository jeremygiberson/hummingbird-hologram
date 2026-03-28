/*
 * SDL2 + GL/GLES2 Hello World — renders a colored triangle in an 800x800 window.
 *
 * macOS:  uses OpenGL 2.1 compatibility profile
 * RPi 5:  uses EGL + GLES2 via SDL2's KMSDRM or X11 backend
 */

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

/* ---------- platform GL headers ---------- */
#ifdef __APPLE__
  #ifndef GL_SILENCE_DEPRECATION
    #define GL_SILENCE_DEPRECATION
  #endif
  #include <OpenGL/gl.h>
#else
  #include <GLES2/gl2.h>
#endif

/* ---------- shader sources ---------- */

#ifdef __APPLE__
  #define VERT_PREAMBLE "#version 120\n"
  #define FRAG_PREAMBLE "#version 120\n"
  #define VARYING_OUT   "varying"
  #define VARYING_IN    "varying"
  #define FRAG_COLOR    "gl_FragColor"
  #define ATTR          "attribute"
#else
  #define VERT_PREAMBLE "precision mediump float;\n"
  #define FRAG_PREAMBLE "precision mediump float;\n"
  #define VARYING_OUT   "varying"
  #define VARYING_IN    "varying"
  #define FRAG_COLOR    "gl_FragColor"
  #define ATTR          "attribute"
#endif

static const char *vert_src =
    VERT_PREAMBLE
    ATTR " vec2 a_pos;\n"
    ATTR " vec3 a_color;\n"
    VARYING_OUT " vec3 v_color;\n"
    "void main() {\n"
    "    v_color = a_color;\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char *frag_src =
    FRAG_PREAMBLE
    VARYING_IN " vec3 v_color;\n"
    "void main() {\n"
    "    " FRAG_COLOR " = vec4(v_color, 1.0);\n"
    "}\n";

/* ---------- helpers ---------- */

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "Shader compile error:\n%s\n", log);
        exit(1);
    }
    return s;
}

static GLuint create_program(const char *vsrc, const char *fsrc) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vsrc);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fsrc);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_pos");
    glBindAttribLocation(prog, 1, "a_color");
    glLinkProgram(prog);
    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "Program link error:\n%s\n", log);
        exit(1);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

/* ---------- main ---------- */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif

    SDL_Window *win = SDL_CreateWindow(
        "Hello Triangle",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        800, 800,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN
    );
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(1); /* vsync */

    printf("GL Vendor:   %s\n", glGetString(GL_VENDOR));
    printf("GL Renderer: %s\n", glGetString(GL_RENDERER));
    printf("GL Version:  %s\n", glGetString(GL_VERSION));

    /* triangle: 3 vertices, each with (x,y) position and (r,g,b) color */
    static const float verts[] = {
     /* x      y      r     g     b   */
        0.0f,  0.6f,  1.0f, 0.0f, 0.0f,  /* top    — red   */
       -0.6f, -0.4f,  0.0f, 1.0f, 0.0f,  /* left   — green */
        0.6f, -0.4f,  0.0f, 0.0f, 1.0f,  /* right  — blue  */
    };

    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    GLuint prog = create_program(vert_src, frag_src);

    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);

    int running = 1;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                running = 0;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
                running = 0;
        }

        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(prog);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(2 * sizeof(float)));

        glDrawArrays(GL_TRIANGLES, 0, 3);

        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);

        SDL_GL_SwapWindow(win);
    }

    glDeleteProgram(prog);
    glDeleteBuffers(1, &vbo);
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
