// script_demo.cpp — demonstrates the ScriptRunner with animated penguins.
//
// Three penguin archetypes:
//   Walkers  — phase-list scripts moving between two waypoints, looping.
//   Bouncers — repeat-rule scripts: launched upward, bounce with damping
//              until momentum runs out, then come to rest.
//   Idlers   — no script; static instances with varied animations.
//
// Keys: C = toggle perspective/ortho  ESC = quit
//
// Test mode env vars match the original demo (CV_TEST_FRAMES, CV_FIXED_TIME,
// CV_SCREENSHOT).
#include <SDL.h>
#include <cmath>
#include <cstdlib>
#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "renderer.h"
#include "script.h"
#include "file_module.h"
#include "screenshot.h"

using namespace cv;

// --- Sprite sheet (same penguin sheet as the stress-test demo) --------------
static const char* SHEET_PATH = "art/penguin.png";
static const int   SHEET_COLS = 16;
static const int   SHEET_ROWS = 1;
static const float ANIM_FPS   = 10.0f;

// Penguin animation ranges on the sheet.
static const int ANIM_WALK_FIRST  = 0, ANIM_WALK_COUNT  = 5;
static const int ANIM_CHAT_FIRST  = 4, ANIM_CHAT_COUNT  = 2;
static const int ANIM_SURP_FIRST  = 6, ANIM_SURP_COUNT  = 1;
static const int ANIM_CONF_FIRST  = 7, ANIM_CONF_COUNT  = 1;
static const int ANIM_ANGR_FIRST  = 8, ANIM_ANGR_COUNT  = 1;

static const int   WINDOW_W  = 1280;
static const int   WINDOW_H  = 720;
static const float AREA      = 14.0f; // world-space extent of the scene
static const float SPRITE_S  = 0.9f;  // sprite size in world units
static const float GRAVITY   = -9.8f;

// ---------------------------------------------------------------------------
// Scene setup helpers
// ---------------------------------------------------------------------------

// Deterministic spread — no rand(). Produces a varied layout using simple math.
static float spread(int i, float scale, float offset) {
    return offset + std::fmod(static_cast<float>(i) * 1.618f * scale, scale);
}

static std::vector<Camera> buildCameras(float angle) {
    Vec3 center = {AREA * 0.5f, 0.0f, AREA * 0.5f};
    float R = AREA * 0.85f;
    float H = AREA * 0.7f;
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
    ortho.orthoHalfHeight = AREA * 0.6f;

    return {persp, ortho};
}

static void buildScene(ScriptRunner& runner, WorldState& state) {
    EntityId id = 1;
    Diff     initDiff; // runner writes initial instances here

    // --- Walkers: phase-list scripts, loop A→B→A -------------------------
    // 12 penguins in two rows, each walking back and forth along x.
    const int   NWALKERS = 12;
    const float WALK_DUR = 2.0f;

    for (int i = 0; i < NWALKERS; ++i) {
        float z     = (i < 6) ? 3.0f : 9.0f;
        float xA    = 1.0f + (i % 6) * 2.0f;
        float xB    = xA + 1.8f;
        float animOff = -(float)i * 0.17f; // stagger walk cycle

        Vec3 posA = {xA, SPRITE_S * 0.5f, z};
        Vec3 posB = {xB, SPRITE_S * 0.5f, z};

        auto makeWalkPhase = [&](Vec3 from, Vec3 to, float dur) {
            Instance inst = makeBillboard(from, {SPRITE_S, SPRITE_S});
            inst.vel      = {(to.x - from.x) / dur, 0.0f, (to.z - from.z) / dur};
            setAnimation(inst, ANIM_WALK_FIRST, ANIM_WALK_COUNT, ANIM_FPS, animOff);
            return Phase{inst, exitAfter(dur)};
        };

        EntityScript ws;
        ws.id     = id++;
        ws.phases = {makeWalkPhase(posA, posB, WALK_DUR),
                     makeWalkPhase(posB, posA, WALK_DUR)};
        ws.loop   = true;
        runner.add(std::move(ws), 0.0f, initDiff);
    }

    // --- Bouncers: repeat-rule scripts, bounce until momentum dies --------
    // 8 penguins scattered across the scene, different initial heights/speeds.
    const int NBOUNCERS = 8;

    for (int j = 0; j < NBOUNCERS; ++j) {
        float x      = spread(j, AREA - 2.0f, 1.0f);
        float z      = spread(j + 3, AREA - 2.0f, 1.0f);
        float initVY = 3.5f + j * 0.4f; // 3.5 … 6.3 m/s upward

        Instance seed = makeBillboard({x, 0.0f, z}, {SPRITE_S, SPRITE_S});
        seed.vel      = {0.0f, initVY, 0.0f};
        seed.accel    = {0.0f, GRAVITY, 0.0f};
        setAnimation(seed, ANIM_SURP_FIRST, ANIM_SURP_COUNT, 0.0f, 0.0f);

        const float DAMPING = 0.6f;
        const float MIN_VEL = 0.4f;

        RepeatRule rule;
        rule.seed = seed;
        rule.exit = exitOnGround(0.0f);
        rule.transition = [DAMPING](const Instance& prev, float t) {
            float dt   = t - prev.motionStart;
            float vyImpact = prev.vel.y + prev.accel.y * dt; // negative at ground hit
            Instance next  = prev;
            next.pos.x    += prev.vel.x * dt + 0.5f * prev.accel.x * dt * dt;
            next.pos.y     = 0.0f;
            next.pos.z    += prev.vel.z * dt + 0.5f * prev.accel.z * dt * dt;
            next.vel.x    += prev.accel.x * dt;
            next.vel.y     = -vyImpact * DAMPING; // bounce up, reduced speed
            next.vel.z    += prev.accel.z * dt;
            next.motionStart = t;
            return next;
        };
        rule.stop = [MIN_VEL](const Instance& inst) {
            return std::abs(inst.vel.y) < MIN_VEL;
        };

        EntityScript bs;
        bs.id        = id++;
        bs.hasRepeat = true;
        bs.repeat    = std::move(rule);
        runner.add(std::move(bs), 0.0f, initDiff);
    }

    // --- Idlers: static instances, no scripts ----------------------------
    // Fill remaining space with penguins doing misc animations.
    const struct { int first; int count; } IDLE_ANIMS[] = {
        {ANIM_CHAT_FIRST, ANIM_CHAT_COUNT},
        {ANIM_CONF_FIRST, ANIM_CONF_COUNT},
        {ANIM_ANGR_FIRST, ANIM_ANGR_COUNT},
    };
    const int NIDLE = 10;
    for (int k = 0; k < NIDLE; ++k) {
        float x = spread(k + 7, AREA - 2.0f, 1.0f);
        float z = spread(k + 11, AREA - 2.0f, 1.0f);
        Instance inst = makeBillboard({x, SPRITE_S * 0.5f, z}, {SPRITE_S, SPRITE_S});
        auto& a = IDLE_ANIMS[k % 3];
        setAnimation(inst, a.first, a.count, ANIM_FPS, -(float)k * 0.23f);
        state.instances[id++] = inst;
    }

    // Apply all script-managed initial instances into the world state.
    for (auto& [eid, inst] : initDiff.upserts)
        state.instances[eid] = inst;

    state.cameras     = buildCameras(0.0f);
    state.activeCamera = 0;
}

// ---------------------------------------------------------------------------
// Loop context
// ---------------------------------------------------------------------------

struct LoopContext {
    SDL_Window*  window;
    Renderer*    renderer;
    ScriptRunner runner;
    FileModule*  module;
    Uint64       startTicks;
    Uint64       fpsWindowStart;
    int          frameCount;
    int          activeCam;
    bool         running;
    // test mode
    int          testFrames;
    float        fixedTime;
    bool         doScreenshot;
    int          framesRendered;
    bool         testDone;
};

static void frame(void* arg) {
    LoopContext* ctx = static_cast<LoopContext*>(arg);

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) ctx->running = false;
        if (ev.type == SDL_KEYDOWN) {
            if (ev.key.keysym.sym == SDLK_ESCAPE) ctx->running = false;
            if (ev.key.keysym.sym == SDLK_c && ev.key.repeat == 0)
                ctx->activeCam = (ctx->activeCam + 1) % 2;
        }
        if (ev.type == SDL_WINDOWEVENT &&
            (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
             ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
            ctx->renderer->setViewport(ev.window.data1, ev.window.data2);
        }
    }

    float t = ctx->fixedTime >= 0.0f
        ? ctx->fixedTime
        : (float)(SDL_GetTicks64() - ctx->startTicks) / 1000.0f;

    // Advance scripts, then update camera orbit — both go into one Diff.
    Diff diff;
    ctx->module->tick(t, ctx->runner, diff);
    ctx->runner.advance(t, diff);
    diff.replaceCameras  = true;
    diff.cameras         = buildCameras(t * 0.08f);
    diff.setActiveCamera = true;
    diff.activeCamera    = ctx->activeCam;
    ctx->renderer->applyDiff(diff);

    ctx->renderer->render(t);

    ctx->framesRendered++;
    bool lastTestFrame = ctx->testFrames > 0
                         && ctx->framesRendered >= ctx->testFrames
                         && !ctx->testDone;
    if (lastTestFrame) {
        ctx->testDone = true;
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) SDL_Log("CV_GLERROR: 0x%x", err);
        if (!framebufferNonBlank(WINDOW_W, WINDOW_H)) SDL_Log("CV_BLANK: framebuffer appears empty");
        if (ctx->doScreenshot) captureFramebufferBase64(WINDOW_W, WINDOW_H);
        ctx->running = false;
#ifdef __EMSCRIPTEN__
        exit(0);
#endif
    }

    SDL_GL_SwapWindow(ctx->window);

    ctx->frameCount++;
    Uint64 now     = SDL_GetTicks64();
    float elapsed  = (float)(now - ctx->fpsWindowStart) / 1000.0f;
    if (elapsed >= 1.0f) {
        float fps = ctx->frameCount / elapsed;
        std::string title = "Clarevoyance script demo  |  "
                          + std::to_string((int)std::round(fps)) + " FPS"
                          + "  (C: toggle camera  ESC: quit)";
        SDL_SetWindowTitle(ctx->window, title.c_str());
        ctx->frameCount     = 0;
        ctx->fpsWindowStart = now;
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int, char**) {
    int   testFrames   = 0;
    float fixedTime    = -1.0f;
    bool  doScreenshot = false;

#ifdef __EMSCRIPTEN__
    {
        char buf[64] = {};
        EM_ASM({
            var p = new URLSearchParams(location.search);
            var v;
            if ((v = p.get('CV_TEST_FRAMES'))) stringToUTF8(v, $0, 64);
            if ((v = p.get('CV_FIXED_TIME')))  stringToUTF8(v, $1, 64);
            if ((v = p.get('CV_SCREENSHOT')))  stringToUTF8(v, $2, 64);
        }, buf, buf + 16, buf + 32);
        if (buf[0])    testFrames   = std::atoi(buf);
        if (buf[16])   fixedTime    = std::atof(buf + 16);
        if (buf[32])   doScreenshot = (std::atoi(buf + 32) != 0);
    }
#else
    if (const char* v = getenv("CV_TEST_FRAMES"))  testFrames   = std::atoi(v);
    if (const char* v = getenv("CV_FIXED_TIME"))   fixedTime    = std::atof(v);
    if (const char* v = getenv("CV_SCREENSHOT"))   doScreenshot = (std::atoi(v) != 0);
#endif

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    SDL_Window* window = SDL_CreateWindow(
        "Clarevoyance script demo",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_W, WINDOW_H, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!window) { SDL_Quit(); return 1; }

    SDL_GLContext glCtx = SDL_GL_CreateContext(window);
    if (!glCtx) { SDL_DestroyWindow(window); SDL_Quit(); return 1; }
    SDL_GL_SetSwapInterval(1);

    Renderer renderer;
    if (!renderer.init(SHEET_PATH, SHEET_COLS, SHEET_ROWS)) {
        SDL_Log("Renderer init failed — is %s present?", SHEET_PATH);
        SDL_GL_DeleteContext(glCtx); SDL_DestroyWindow(window); SDL_Quit();
        return 1;
    }
    renderer.setViewport(WINDOW_W, WINDOW_H);

    // Build the scene (static idlers into WorldState, scripted entities via runner).
    ScriptRunner runner;
    WorldState   state;
    buildScene(runner, state);
    renderer.applyState(state);

    FileModule module("scenes/demo.json");

    Uint64 startTicks = SDL_GetTicks64();
    // Heap-allocated so the pointer stays valid after emscripten_set_main_loop_arg
    // unwinds the C stack (simulate_infinite_loop=1).
    LoopContext* ctx = new LoopContext{
        window, &renderer, std::move(runner), &module,
        startTicks, startTicks,
        0, 0, true,
        testFrames, fixedTime, doScreenshot, 0, false
    };

#ifdef __EMSCRIPTEN__
    const int loopFps = 0; // 0 = use browser's requestAnimationFrame rate
    emscripten_set_main_loop_arg(frame, ctx, loopFps, 1);
#else
    while (ctx->running) frame(ctx);
    renderer.shutdown();
    SDL_GL_DeleteContext(glCtx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    delete ctx;
#endif
    return 0;
}
