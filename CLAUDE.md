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
- Grid-locked movement and rotation, fixed to 90° increments only (no 45° diagonals)
- Smooth transitions: camera lerps between grid positions and angles, never snaps
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
- Large open rooms may trigger overhead camera (exact trigger TBD — automatic or manual)

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
  STORY.md      — full narrative, character arcs, boss designs, themes
  CLAUDE.md     — this file
```

---

## Build & Run

*(To be completed once project is initialized)*

```bash
# Placeholders — fill in after first session
make build
make run

# WebAssembly build (browser target)
make build-wasm
```

---

## Coding Conventions

*(To be expanded as patterns emerge — add decisions here as they're made)*

- Prefer clear, readable C over clever C
- Engine systems must be decoupled from game logic
- Camera logic lives in `/engine`, not `/game`
- Clairvoyance shim rendering lives in `/engine` (it's a visual system), triggered by `/game`
- No magic numbers — named constants for grid size, turn speed, shim opacity, etc.
- All game simulation logic must be deterministic — document any RNG usage with its seed

---

## Hard Constraints — Do Not Change Without Discussion

- Third-person camera is non-negotiable — Clare must always be visible on screen
- No minimap under any circumstances
- Movement and rotation locked to 90° grid increments
- Shim system must be a 3D world visual effect, not a HUD/UI element
- Engine and game logic must remain in separate layers
- Game simulation must remain fully deterministic (required by shim lookahead system)
- Shader-based frame animation must be preserved — do not move animation to CPU
- Do not refactor core engine architecture without explicit instruction

---

## Current Status

Pre-production — no code written yet.

**First session goal:** Minimal C++ project with SDL2 and OpenGL that renders a
billboarded sprite with frame animation handled entirely in the shader.
