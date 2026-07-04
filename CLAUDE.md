# CLAREVOYANCE — Project Context for AI Agents

For full narrative and character context, see [STORY.md](docs/STORY.md).

---

## What This Is

Clarevoyance is an action RPG built in C/C++ using SDL2 and OpenGL, targeting desktop,
browser (via WebAssembly/Emscripten), and Nintendo Switch. The game features a
third-person perspective, grid-based movement with smooth transitions, and a
sprite-heavy aesthetic inspired by DOOM-era engines.

The game's central mechanic mirrors the protagonist's supernatural gift: players see
translucent "shims" (ghosted previews) of incoming attacks moments before they occur.
Upgrading Clare's foresight is the entire RPG progression system — every upgrade
changes how the game *feels*, not just what numbers say.

---

## Characters

**Clare** — protagonist, young woman with clairvoyance since birth. She sees both
long-range visions and split-second foresight. Her power is the core RPG progression
system: upgrades widen the preview window and improve shim clarity.

**Desiris Winter** — ice witch, major antagonist who becomes a reluctant ally. Complex
character with a significant narrative arc. Not a physical fighter — relies on powerful
magic and cunning. More magically powerful than Clare. Has a backstory involving a
hero named Aiden and a lost past that quietly defines her throughout.

**Hiri** — the Hands of God. Undefeated champion. Appears to have telekinesis. Secret:
he has no power at all — he is protected by two invisible helpers and is a masterful
deceiver.

**Mochi** — a small cat with ancient, knowing eyes, carried by an ordinary oblivious
man. The cat is the real wizard; the man is an unwitting puppet. Mochi was never evil
and eventually switches to the good side. Clare helps him find his way home to his
family before continuing her mission.

**Spirolo** — impossibly fast, a blur of lethal accuracy. Weakness: water slows him
significantly, making him manageable. Stopped entirely when Desiris freezes water
around him.

**The Twin Dolls** — two sisters, unnamed, who appear as enormous figures in deep
crimson and black Japanese doll kimonos on either side of a long dark bridge. Porcelain
white faces with painted red lips in fixed grins. When Clare reaches the end of the
bridge they are revealed to be two remarkably short girls — same faces, same kimonos,
equally dangerous. Their attack pattern is perfectly mirrored — dodging one leads into
the other.

**Amblico** — the Mindbender. Clare's closest childhood friend, now her most personally
threatening enemy. His power corrupts her clairvoyance directly — showing false shims
and hiding real attacks. Secret: he turned evil under the influence of his younger
sister Skivana.

**Skivana** — Amblico's younger sister. Has hated Clare since childhood out of jealousy.
Deliberately influenced Amblico's fall. Her power warps all perception: gravity,
sound, touch, images — everything becomes unreliable except Clare's gift itself.

**Dr. Mad** — a one-off, mysterious antagonist who belongs to no faction and is
connected to no one. Anachronistic — a scientist operating entirely outside the
fantasy world's logic. Captures Clare and Desiris mid-battle and subjects them to
experiments in his lab, transporting them to an illusory version of the modern world.

**King Dev** — the final boss. No magical power. His rare and dangerous gift is a
complete understanding of how Clare thinks — he fakes attacks, manipulates her shims
psychologically, and speaks to her during combat to find where doubt lives. Responsible
for Clare's father's death. His unchecked ambitions would fracture the kingdom.

---

## Tech Stack

- **Language:** C/C++
- **Windowing / Input / Audio:** SDL2 (Switch-compatible)
- **Low-level Graphics:** OpenGL (desktop) / WebGL via Emscripten (browser)
- **Sprite Art:** Aseprite
- **Target Platforms:** Desktop, Browser (WebAssembly), Nintendo Switch

---

## Core Architecture Decisions

### Camera System
- Third-person perspective — the player character is always visible on screen
- Camera rotation is locked to 90° increments only (no 45° diagonals)
- Character/player movement is FREE — not grid-locked; the 90° lock is a camera constraint, not a movement one
- Smooth transitions: camera lerps between positions and angles, never snaps
- Dynamic camera shift: large open dungeon rooms may transition to overhead/isometric view
- The smooth turn system is non-negotiable — do not simplify to instant rotation

### World Structure

**Outdoors:**
- Long linear roads with forks — not a maze
- Forks lead to different areas; the road ahead is always the primary path
- No minimap — navigation is by landmark and memory
- Pacing is deliberate: long stretches build tension before encounters or decision points

**Dungeons:**
- More complex layouts than outdoors
- Third-person maintained throughout
- Large open rooms trigger the overhead camera automatically based on room size, with a manual player override

### Rendering Philosophy
- Sprite-heavy: characters, enemies, and NPCs are billboard sprites in a 3D world
- Sprites always face the camera (billboarding)
- Environment is 3D geometry; characters are 2D sprites — DOOM-engine-inspired aesthetic
- Frame-by-frame sprite animation is handled entirely in the shader — no extra draw
  calls per frame. Animation state (frame index, timing) is passed as uniforms;
  the GPU selects the correct frame. This is a core engine optimization — preserve it.

### Data-Driven Architecture
- Game logic (worlds, enemies, dialogue, events) is defined in data files, not code
- The C++ engine is a black box that runs any game defined by those files
- This allows future games to be built on the same engine with no engine changes
- Event/trigger logic (e.g. "when player talks to NPC, check inventory, unlock door")
  is handled via a data-driven event/condition system — exact format TBD

### Tile Caching / World Generation
- A spatial caching system loads surrounding tiles and discards distant ones
- Designed to support infinite or very large world generation from data definitions
- Keep this system simple — complexity here caused problems in prior prototype

### Clairvoyance Mechanic (Core Gameplay System)
- Translucent "shims" appear as ghost outlines of incoming enemy attacks before they land
- Players read shims and position Clare to dodge — this is the primary gameplay loop
- Shims are a visual system rendered in the 3D world, not a UI overlay
- Shim clarity and preview time expand as Clare's power upgrades — this IS the RPG system
- Every upgrade should change how the game *feels*, not just what numbers say

### Shim System — Deterministic Simulation Requirement
- The shim system works by running a lookahead simulation of the game state N seconds
  ahead, then rendering future entity positions as translucent ghosts in the present frame
- **The entire game simulation must be deterministic from day one** — same inputs always
  produce same outputs. No frame-rate-dependent physics. Seeded RNG only.
- The lookahead pass is a simplified simulation: no player collision, no full physics —
  just enough to predict enemy attack positions
- Enemy AI must be deterministic by design — this is non-negotiable

---

## Folder Structure

```
/src
  /engine       — core engine systems (renderer, camera, entity manager, collision)
  /game         — game-specific logic (combat, clairvoyance system, enemy AI)
  /levels       — level data, dungeon layouts, road definitions
/assets
  /sprites      — character and enemy billboard sprites (Aseprite source + exports)
  /textures     — environment textures and tiles
  /audio        — music and sound effects
/docs
  STORY.md          — full narrative, character arcs, boss designs, themes
  ARCHITECTURE.md   — engine layer: renderer contract, Instance/WorldState/Diff/Camera API
  GAME_LAYER.md     — game layer: combat, AI, clairvoyance system, event format, world structure
  /specs            — requirements specs (scene coordinator, world building, map editor, shims)
CLAUDE.md           — this file (repo root)
```

---

## Build & Run

### Prerequisite (WebAssembly only)

The Emscripten toolchain must be sourced before any WASM target:

```bash
source ~/emsdk/emsdk_env.sh   # adds emcc / emrun to PATH for the session
```

Install once if needed:
```bash
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest
```

### Desktop

```bash
make build    # compile → build/clarevoyance
make run      # bundle into build/Clarevoyance.app and open it
make clean    # remove build/ and tools/imgdiff
```

### WebAssembly (browser)

```bash
make build-wasm   # compile → build/web/index.{html,js,wasm,data}
make run-wasm     # build-wasm + emrun (serves on localhost, opens browser)
```

### Automated tests

```bash
make test          # desktop: render 120 frames at t=2.0s, capture screenshot
make test-wasm     # web: same via emrun + Chrome (closes automatically)
make test-parity   # pixel-diff desktop.png vs web.png — exits 1 if images diverge
```

Screenshots land in `build/test/` (`desktop.png`, `web.png`).
Test config is controlled by `TEST_FRAMES` and `TEST_TIME` in the Makefile.

### Deploy (Cloudflare Pages)

The live site is hosted at **https://clare.dobuki.net** (also https://clarevoyance.pages.dev).

Cloudflare Pages is configured with no build command — WASM must be pre-built locally because
Emscripten is not available on Cloudflare's build machines. The built output is committed to
`docs-web/` and Cloudflare serves it directly.

```bash
# Full deploy (build + commit + push → CF auto-deploys)
source ~/emsdk/emsdk_env.sh && make build-wasm
mkdir -p docs-web && cp build/web/* docs-web/
git add docs-web/ && git commit -m "Deploy: update WASM build" && git push origin main
```

Or use the `/deploy` slash command in Claude Code.

**Cloudflare config:**
- Pages project: `clarevoyance`
- Account: Vincent (`1fe1ef92444f52ef8d7ff09c175d034e`)
- dobuki.net zone: `849149529f0b407bc3215d3f0986d08d`
- Output directory: `docs-web/`
- Dashboard: https://dash.cloudflare.com/1fe1ef92444f52ef8d7ff09c175d034e/pages/view/clarevoyance

**Cloudflare MCP (for AI agents):**
The `cloudflare-api` MCP plugin is configured in `~/.claude/settings.json` globally. It authenticates
via OAuth and exposes `mcp__plugin_cloudflare_cloudflare-api__execute` for direct API calls.
The plugin is from the `cloudflare/skills` marketplace (`cloudflare@cloudflare` in `enabledPlugins`).
If the MCP needs re-authentication, call `mcp__plugin_cloudflare_cloudflare-api__authenticate`.

---

## GitHub Push — Known Issue with Remote Sessions

**`git push` may fail with 403 in Slack-initiated or other remote sessions.** The built-in
git proxy is provisioned read-only in these environments. This is a known limitation.

**Workaround:** Use the `GITHUB_PAT` environment variable (a classic GitHub PAT with `repo` scope).

- If `GITHUB_PAT` is set, use the `/push-github` skill — it will push via the token and restore
  the original remote URL afterward.
- If `GITHUB_PAT` is not set, ask the user: *"The git proxy is read-only in this session.
  Can you provide a classic GitHub PAT (github.com/settings/tokens, repo scope) so I can push?"*
  Once they paste it, use it as `GITHUB_PAT` and invoke `/push-github`.

The user's local `~/.claude/settings.json` should have:
```json
{ "env": { "GITHUB_PAT": "<their token>" } }
```
This makes it available automatically in local sessions. Remote sessions need it re-entered
each time (or configured in the environment at code.claude.com).

The `/push-github` skill documents the full procedure.

---

## Agent Working Style — Be Proactive

Agents working on this repo should drive tasks to completion without waiting to be asked:

- **Finish the loop.** After pushing, verify CI (`Build and Deploy WASM` workflow) actually
  succeeds. If it fails, read the logs, fix, and push again — don't report a failure and stop.
- **Deploys must land.** A change isn't done until it's visible at https://clare.dobuki.net
  (the GitHub Action builds WASM → commits `docs-web/` → Cloudflare auto-deploys).
- **Fix small blockers autonomously** (build errors, missing files, path issues, CI config).
  Only stop to ask when the decision is architectural, destructive, or changes scope.
- **Keep demos isolated** — one demo per layer/feature (`make demo-events`, `demo-controls`,
  `demo-menu`, stress test). When adding a layer, add its demo and a card in `web/landing.html`.
- **Scenes live ONLY in `src/levels/`** — the WASM build maps them into the virtual FS as
  `scenes/` via `--preload-file src/levels@scenes`, so web URLs keep using `scenes/...` paths.
  There is no separate `scenes/` directory to sync.
- **Verify before claiming done**: run `make build` (and `make test` when relevant) before
  every push.

---

## Coding Conventions

*(To be expanded as patterns emerge — add decisions here as they're made)*

- Prefer clear, readable C over clever C
- Engine systems must be decoupled from game logic
- Camera logic lives in `/engine`, not `/game`
- Clairvoyance shim rendering lives in `/engine` (it's a visual system), triggered by `/game`
- No magic numbers — named constants for grid size, turn speed, shim opacity, etc.
- All game simulation logic must be deterministic — document any RNG usage with its seed
- **Doc sync rule:** any change to `Instance`, `WorldState`, `Diff`, or `Camera` must update `docs/ARCHITECTURE.md` first. `docs/GAME_LAYER.md` references those contracts and must not duplicate or contradict them. When the two docs conflict, `ARCHITECTURE.md` is authoritative.

---

## Hard Constraints — Do Not Change Without Discussion

- Third-person camera is non-negotiable — Clare must always be visible on screen
- No minimap under any circumstances
- Camera rotation locked to 90° increments; character/player movement is free (not grid-locked)
- Shim system must be a 3D world visual effect, not a HUD/UI element
- Engine and game logic must remain in separate layers
- Game simulation must remain fully deterministic (required by shim lookahead system)
- Shader-based frame animation must be preserved — do not move animation to CPU
- Do not refactor core engine architecture without explicit instruction

---

## Current Status

The engine core and a data-driven script layer are implemented and live at
https://clare.dobuki.net (landing page with demo cards).

**What exists:**
- Renderer: single-draw-call instanced billboard sprites, motion + frame animation
  evaluated entirely on the GPU from `uTime` (see `docs/ARCHITECTURE.md`)
- Per-instance `tint` (RGBA multiplier) — the rendering hook for translucent shims
- Data-driven scenes (`src/levels/*.json`): entities, cameras, events
  (trigger/condition/action), key bindings, free player movement
- Scene hot-reload on desktop: edit the JSON while `make demo` runs and it reloads
- Cross-platform: desktop (OpenGL 3.3 Core) and browser (WebGL 2) with pixel-identical
  output verified by `make test-parity`

**Make targets:** `build`, `run`, `demo` (SCENE=…), `demo-events`, `demo-controls`,
`demo-menu`, `build-wasm`, `run-wasm`, `deploy`, `test`, `test-wasm`, `test-parity`, `clean`.

**Next up:** see `docs/specs/` — scene coordinator, world building (textured grid quads),
in-engine map editor, and the shim lookahead system (60 Hz fixed tick, lockstep-friendly).
