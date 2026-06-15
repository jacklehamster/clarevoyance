// main.cpp — Milestone 1 demo for the Clarevoyance graphics engine.
//
// Exercises every feature of the state-driven renderer:
//   * oriented sprites (flat quads yawed about world-up) with frame animation
//   * a billboard sprite that always faces the camera
//   * a projectile whose arc is computed on the GPU from per-instance motion
//     params — re-launched via a Diff only when it lands (no per-frame state)
//   * two cameras (perspective + orthographic) toggled with a key
//
// High-level loop: build WorldState -> applyState -> { applyDiff?; render }.
#include <SDL.h>
#include <cmath>

#include "renderer.h"

using namespace cv;

// --- Sprite sheet config (swap in your own PNG + grid here) ----------------
static const char* SHEET_PATH = "assets/sprites/demo_sheet.png";
static const int SHEET_COLS = 4;
static const int SHEET_ROWS = 1;
static const float ANIM_FPS = 6.0f;

static const int WINDOW_W = 1280;
static const int WINDOW_H = 720;

static const float PI = 3.14159265f;

// Entity ids.
enum : EntityId { PROJECTILE = 1, FIRST_SPRITE = 10, BILLBOARD = 20 };

// Projectile launch params (chosen so it arcs and lands back at start height).
static const Vec3 PROJ_START = {-4.0f, 0.5f, 2.0f};
static const Vec3 PROJ_VEL = {2.5f, 6.0f, 0.0f};
static const Vec3 PROJ_ACCEL = {0.0f, -9.8f, 0.0f};

static Instance makeProjectile(float launchTime) {
    Instance i = makeBillboard(PROJ_START, {1.0f, 1.0f});
    setMotion(i, PROJ_VEL, PROJ_ACCEL, launchTime);
    setAnimation(i, 0, SHEET_COLS * SHEET_ROWS, ANIM_FPS, launchTime);
    return i;
}

static std::vector<Camera> buildCameras(float angle) {
    const float R = 9.0f, H = 4.0f;
    Vec3 eye = {std::sin(angle) * R, H, std::cos(angle) * R};
    Vec3 target = {0.0f, 1.5f, 0.0f};

    Camera persp;
    persp.projection = Projection::Perspective;
    persp.position = eye;
    persp.target = target;

    Camera ortho;
    ortho.projection = Projection::Orthographic;
    ortho.position = eye;
    ortho.target = target;
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
        "Clarevoyance — Engine Demo (C: toggle camera, ESC: quit)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_W, WINDOW_H, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
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
    SDL_GL_SetSwapInterval(1);
    SDL_Log("OpenGL: %s", glGetString(GL_VERSION));

    Renderer renderer;
    if (!renderer.init(SHEET_PATH, SHEET_COLS, SHEET_ROWS)) {
        SDL_Log("Renderer init failed (is %s present?)", SHEET_PATH);
        SDL_GL_DeleteContext(ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    renderer.setViewport(WINDOW_W, WINDOW_H);

    // --- Build the initial world ------------------------------------------
    WorldState state;
    state.cameras = buildCameras(0.0f);
    state.activeCamera = 0;

    // A row of oriented sprites, each yawed differently so the orbiting camera
    // catches them face-on and edge-on.
    const int kSprites = 4;
    for (int i = 0; i < kSprites; ++i) {
        float x = -3.0f + i * 2.0f;
        Instance s = makeSprite({x, 1.0f, 0.0f}, {2.0f, 2.0f}, i * (PI / 4.0f));
        setAnimation(s, 0, SHEET_COLS * SHEET_ROWS, ANIM_FPS, 0.0f);
        state.instances[FIRST_SPRITE + i] = s;
    }

    // A billboard sprite floating above, always facing the camera.
    Instance bb = makeBillboard({0.0f, 3.5f, 0.0f}, {2.0f, 2.0f});
    setAnimation(bb, 0, SHEET_COLS * SHEET_ROWS, ANIM_FPS, 0.0f);
    state.instances[BILLBOARD] = bb;

    // The projectile.
    state.instances[PROJECTILE] = makeProjectile(0.0f);

    renderer.applyState(state);

    // Time to return to launch height: -2*vy / ay.
    const float flightTime = -2.0f * PROJ_VEL.y / PROJ_ACCEL.y;

    bool running = true;
    int activeCam = 0;
    float projectileLaunch = 0.0f;
    SDL_Event event;
    Uint64 startTicks = SDL_GetTicks64();

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

        // Per-frame: move the cameras (cheap — just uniforms) and re-launch the
        // projectile only when it lands (the one moment its trajectory changes).
        Diff diff;
        diff.replaceCameras = true;
        diff.cameras = buildCameras(t * 0.3f);
        diff.setActiveCamera = true;
        diff.activeCamera = activeCam;

        if (t - projectileLaunch >= flightTime) {
            projectileLaunch = t;
            diff.upserts.push_back({PROJECTILE, makeProjectile(t)});
        }

        renderer.applyDiff(diff);
        renderer.render(t);
        SDL_GL_SwapWindow(window);
    }

    renderer.shutdown();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
