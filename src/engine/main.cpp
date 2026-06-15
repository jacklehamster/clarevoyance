#include <SDL.h>
#include <OpenGL/gl3.h>
#include <cstdio>
#include <cmath>

static const int WINDOW_W = 1280;
static const int WINDOW_H = 720;

// -----------------------------------------------------------------------
// Shaders
// -----------------------------------------------------------------------

static const char* VERT_SRC = R"glsl(
#version 330 core

layout(location = 0) in vec2 aPos;   // -1..1 NDC quad
layout(location = 1) in vec2 aUV;

out vec2 vUV;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUV = aUV;
}
)glsl";

// Cycles through a 4x1 sprite sheet using time, drawn as a tinted quad.
static const char* FRAG_SRC = R"glsl(
#version 330 core

in  vec2 vUV;
out vec4 fragColor;

uniform float uTime;
uniform int   uFrameCount; // total columns in the sprite sheet
uniform int   uFrame;      // current frame index (set by CPU or GPU-computed)

void main() {
    // Map uv.x into the current frame's column
    float col   = float(uFrame);
    float total = float(uFrameCount);
    float u     = (col + vUV.x) / total;

    // Simple checkerboard pattern to stand in for a real sprite texture
    float checker = mod(floor(u * 16.0) + floor(vUV.y * 16.0), 2.0);
    vec3 baseColor = mix(vec3(0.15, 0.55, 0.85), vec3(0.9, 0.9, 0.9), checker);

    // Tint shifts each frame so you can see animation is running
    float hue = col / total;
    vec3 tint = 0.5 + 0.5 * vec3(cos(hue * 6.28),
                                   cos(hue * 6.28 + 2.09),
                                   cos(hue * 6.28 + 4.19));
    fragColor = vec4(baseColor * tint, 1.0);
}
)glsl";

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

static GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, 512, nullptr, log);
        SDL_Log("Shader compile error: %s", log);
    }
    return s;
}

static GLuint buildProgram(const char* vert, const char* frag) {
    GLuint v = compileShader(GL_VERTEX_SHADER, vert);
    GLuint f = compileShader(GL_FRAGMENT_SHADER, frag);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, 512, nullptr, log);
        SDL_Log("Program link error: %s", log);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

// -----------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------

int main(int /*argc*/, char* /*argv*/[]) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // Request OpenGL 3.3 Core
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    SDL_Window* window = SDL_CreateWindow(
        "Clarevoyance — Hello World",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_W, WINDOW_H,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN
    );
    if (!window) {
        SDL_Log("Window creation failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    if (!ctx) {
        SDL_Log("GL context creation failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(1); // vsync

    SDL_Log("OpenGL: %s", glGetString(GL_VERSION));
    SDL_Log("Renderer: %s", glGetString(GL_RENDERER));

    // Full-screen quad (two triangles, NDC)
    //   position (xy)   uv
    float verts[] = {
        -0.5f,  0.5f,   0.0f, 1.0f,
        -0.5f, -0.5f,   0.0f, 0.0f,
         0.5f, -0.5f,   1.0f, 0.0f,
         0.5f,  0.5f,   1.0f, 1.0f,
    };
    unsigned int indices[] = { 0, 1, 2,  0, 2, 3 };

    GLuint vao, vbo, ebo;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    GLuint program = buildProgram(VERT_SRC, FRAG_SRC);

    const int FRAME_COUNT = 4;
    const float FRAME_DURATION = 0.2f; // seconds per frame

    bool running = true;
    SDL_Event event;
    Uint64 startTicks = SDL_GetTicks64();

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
                running = false;
        }

        float t = (float)(SDL_GetTicks64() - startTicks) / 1000.0f;
        int frame = (int)(t / FRAME_DURATION) % FRAME_COUNT;

        glClearColor(0.08f, 0.08f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(program);
        glUniform1f(glGetUniformLocation(program, "uTime"), t);
        glUniform1i(glGetUniformLocation(program, "uFrameCount"), FRAME_COUNT);
        glUniform1i(glGetUniformLocation(program, "uFrame"), frame);

        glBindVertexArray(vao);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);

        SDL_GL_SwapWindow(window);
    }

    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &ebo);
    glDeleteProgram(program);
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
