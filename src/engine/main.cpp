// main.cpp — stress-test demo for the Clarevoyance graphics engine.
//
// Spawns a large grid of penguin billboard sprites (GRID_W × GRID_H) in a
// single applyState call. After that, nothing touches the instance buffer —
// animation runs entirely in the shader via uTime. FPS is shown in the title.
//
// Keys: C = toggle perspective/ortho  ESC = quit
#include <SDL.h>
#include <cmath>
#include <string>

#include "renderer.h"

using namespace cv;

// --- Sprite sheet -----------------------------------------------------------
// penguin.png: 4096×256, 16 cols × 1 row of 256×256 cells.
//   Walk(0-4)  Chat(4-5)  Surprised(6)  Confused(7)  Angry(8)
static const char* SHEET_PATH = "art/penguin.png";
static const int   SHEET_COLS = 16;
static const int   SHEET_ROWS = 1;
static const float ANIM_FPS   = 10.0f;

// --- Stress-test grid -------------------------------------------------------
static const int   GRID_W     = 100;   // penguins wide
static const int   GRID_H     = 100;   // penguins deep
static const float GRID_STEP  = 1.2f;  // world units between penguins

static const int WINDOW_W = 1280;
static const int WINDOW_H = 720;

// Five animation flavours to cycle through so the grid looks varied.
static const struct { int first; int count; } ANIMS[] = {
    {0, 5},  // Walk
    {4, 2},  // Chat
    {6, 1},  // Surprised
    {7, 1},  // Confused
    {8, 1},  // Angry
};
static const int NANIM = 5;

static std::vector<Camera> buildCameras(float angle) {
    // Fixed view distance — always shows ~12 penguins across regardless of
    // how large the grid is. The rest of the grid extends out of frame.
    const float R  = 8.0f;
    const float H  = 6.0f;
    Vec3 center = {
        (GRID_W - 1) * GRID_STEP * 0.5f,
        0.5f,
        (GRID_H - 1) * GRID_STEP * 0.5f
    };
    Vec3 eye = {
        center.x + std::sin(angle) * R,
        H,
        center.z + std::cos(angle) * R
    };

    Camera persp;
    persp.projection = Projection::Perspective;
    persp.position   = eye;
    persp.target     = center;

    Camera ortho;
    ortho.projection      = Projection::Orthographic;
    ortho.position        = eye;
    ortho.target          = center;
    ortho.orthoHalfHeight = 5.0f;

    return {persp, ortho};
}

int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    SDL_Window* window = SDL_CreateWindow(
        "Clarevoyance — stress test",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_W, WINDOW_H, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!window) { SDL_Quit(); return 1; }

    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    if (!ctx) { SDL_DestroyWindow(window); SDL_Quit(); return 1; }
    SDL_GL_SetSwapInterval(1);
    SDL_Log("OpenGL: %s", glGetString(GL_VERSION));

    Renderer renderer;
    if (!renderer.init(SHEET_PATH, SHEET_COLS, SHEET_ROWS)) {
        SDL_Log("Renderer init failed — is %s present?", SHEET_PATH);
        SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(window); SDL_Quit();
        return 1;
    }
    renderer.setViewport(WINDOW_W, WINDOW_H);

    // --- Build the world once -----------------------------------------------
    WorldState state;
    state.cameras     = buildCameras(0.0f);
    state.activeCamera = 0;

    const int total = GRID_W * GRID_H;
    SDL_Log("Spawning %d penguins…", total);

    EntityId id = 1;
    for (int row = 0; row < GRID_H; ++row) {
        for (int col = 0; col < GRID_W; ++col, ++id) {
            float x = col * GRID_STEP;
            float z = row * GRID_STEP;
            // Stagger animation start so they're not all in lock-step.
            float animOffset = -(col + row * GRID_W) * (1.0f / ANIM_FPS);
            int aIdx = (col + row) % NANIM;

            Instance inst = makeBillboard({x, 0.5f, z}, {1.0f, 1.0f});
            setAnimation(inst, ANIMS[aIdx].first, ANIMS[aIdx].count,
                         ANIM_FPS, animOffset);
            state.instances[id] = inst;
        }
    }

    renderer.applyState(state);  // one upload — never touched again
    SDL_Log("Buffer uploaded. Rendering %d instances per frame.", total);

    // --- Render loop --------------------------------------------------------
    bool running   = true;
    int  activeCam = 0;
    SDL_Event event;
    Uint64 startTicks = SDL_GetTicks64();

    // FPS tracking
    Uint64 fpsWindowStart = startTicks;
    int    frameCount     = 0;

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE) running = false;
                if (event.key.keysym.sym == SDLK_c && event.key.repeat == 0)
                    activeCam = (activeCam + 1) % 2;
            }
        }

        float t = (float)(SDL_GetTicks64() - startTicks) / 1000.0f;

        // Only thing that changes each frame: the camera orbit (uniforms only).
        Diff diff;
        diff.replaceCameras  = true;
        diff.cameras         = buildCameras(t * 0.1f);
        diff.setActiveCamera = true;
        diff.activeCamera    = activeCam;
        renderer.applyDiff(diff);

        renderer.render(t);
        SDL_GL_SwapWindow(window);

        // Update title with FPS once per second.
        ++frameCount;
        Uint64 now = SDL_GetTicks64();
        float elapsed = (float)(now - fpsWindowStart) / 1000.0f;
        if (elapsed >= 1.0f) {
            float fps = frameCount / elapsed;
            std::string title =
                "Clarevoyance — " + std::to_string(total) + " penguins  |  " +
                std::to_string((int)std::round(fps)) + " FPS" +
                "  (C: toggle camera  ESC: quit)";
            SDL_SetWindowTitle(window, title.c_str());
            frameCount    = 0;
            fpsWindowStart = now;
        }
    }

    renderer.shutdown();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
