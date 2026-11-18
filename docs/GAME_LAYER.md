# Clarevoyance — Game Layer Architecture

This document covers everything *above* the engine API. For the renderer contract
(`Instance`, `WorldState`, `Diff`, `Camera`), see [ARCHITECTURE.md](ARCHITECTURE.md).
The game layer builds `Diff`s and hands them to the renderer — it never calls GL directly.

---

## Roadmap specs

Requirements documents for the next major systems live in `docs/specs/`:

- [SCENE_COORDINATOR.md](specs/SCENE_COORDINATOR.md) — runtime scene switching, `change_scene` + spawn points, persistent GameState vs scene-local state.
- [WORLD_BUILDING.md](specs/WORLD_BUILDING.md) — floor/wall tile layer, spatial tile cache, roads with forks, collision, landmark navigation.
- [MAP_EDITOR.md](specs/MAP_EDITOR.md) — strict versioned scene schema, JSON round-trip serializer, archetypes and named clips, editor tooling.
- [SHIM_SYSTEM.md](specs/SHIM_SYSTEM.md) — fixed-timestep deterministic sim, lookahead fork, translucent shim render pass, upgrade tiers, corruption effects.

---

## Responsibilities

| Layer | Owns |
|-------|------|
| Engine (`src/engine/`) | Rendering, camera, billboarding, shader animation, shim visual pass |
| Game (`src/game/`) | Combat, enemy AI, clairvoyance logic, event system, world/level loading |

---

## Core Gameplay Loop

The sim advances in fixed **60 Hz ticks** (`SIM_DT = 1/60 s`, `src/game/sim.h`);
the engine loop runs an accumulator over wall time (see ARCHITECTURE.md,
Fixed-Timestep Simulation Loop). Per rendered frame:

```
1. Engine translates SDL events → InputFrame (once per frame; game layer is SDL-free)
2. accumulator += wall dt (capped);  while accumulator >= SIM_DT:
     a. Resolve the tick-stamped input command → ResolvedActions (pure function;
        pressed/released edges are consumed by the first tick, never repeated)
     b. stepSim(scene, simState, actions, diff):
          - applyMovement to controlled entities
          - evaluate events (triggers/conditions/actions)
          - upsert entities whose trajectory/animation changed, remove despawns
          - simState.clock.tick++
     c. accumulator -= SIM_DT
3. (future) Run clairvoyance lookahead: copy SimState, step the copy N/SIM_DT
   ticks, upsert shim ghosts from the result
4. renderer.applyDiff(diff)
5. renderer.render(clock.time() + accumulator)   ← continuous time, smooth motion
```

Steps 1–3 are pure CPU simulation over an immutable `Scene` plus a copyable
`SimState` (all runtime state: entities, attrs, flags, fired markers, clock).
Steps 4–5 are engine calls. The game layer never touches a VBO or uniform
directly — and never sees an SDL type.

---

## Entity Model

Each game entity maps to one or more `Instance`s in the renderer. The game layer owns
entity identity and state; the renderer owns the visual representation.

```
Entity (game layer)
  ├── EntityId  → live Instance  (current sprite)
  └── EntityId  → shim Instance  (clairvoyance ghost, if visible)
```

Entity IDs are assigned by the game layer. Odd IDs = live sprites; the paired even ID
= shim ghost — or use a separate ID namespace. TBD during implementation.

---

## Clairvoyance Shim System

### Concept

Shims are translucent ghost sprites that show where enemies will be N seconds from now.
They are normal `Instance`s rendered with reduced opacity (future: `tint` field on
`Instance`, or a second draw pass with alpha uniform). The engine renders them; the
game layer computes them.

### Lookahead Simulation

The game runs a simplified forward simulation N seconds ahead:

- Inputs: current entity positions, velocities, AI state machines (no player collision)
- Outputs: predicted positions and animation states at `now + N`
- Collision detection: AABB only, enough to know when an attack arc terminates

Because the simulation is fully deterministic, the lookahead produces stable,
non-flickering shims. Same sim state + same N → same ghost positions every frame.

### Encoding Shims as Instances

A shim is an Instance whose motion is set to the *predicted* future trajectory:

```cpp
// Live entity at current position
Instance live = makeBillboard(currentPos, scale);
setAnimation(live, walkFirst, walkCount, fps, animStart);

// Shim: same trajectory but motionStart is in the future
Instance shim = live;
shim.motionStart = now + lookaheadSeconds;
// tint = translucent (TBD)
```

The GPU evaluates both using the identical motion formula. No second render path needed.

### Clare's Upgrades

Upgrades expand `lookaheadSeconds` and improve shim clarity (opacity, detail level).
This is the entire RPG progression system — every upgrade changes how the game *feels*.

| Upgrade tier | Lookahead | Effect |
|-------------|-----------|--------|
| 0 (base) | 0.5s | faint outlines only |
| 1 | 1.0s | clearer silhouettes |
| 2 | 2.0s | full colour ghosts |
| 3 | 3.5s | attack arc previewed |

Exact values TBD during tuning.

---

## Enemy AI

### Requirements

- All AI must be **deterministic**: state transitions are functions of sim time and
  observable game events only. No `rand()` without a seeded RNG.
- AI is defined in data files where possible (see Event System below).
- Complex bosses (Hiri, King Dev) may have hand-coded C++ AI modules in `src/game/ai/`.

### State Machine Pattern

Each enemy has a simple state machine. Transitions fire at sim-time events:

```
Idle → Approach (when player enters radius)
     → Attack   (when in range, cooldown elapsed)
     → Retreat  (when health < threshold)
Attack → Wind-up → Strike → Recovery → Idle
```

Wind-up and Strike phases are what the shim system previews. The lookahead sim only
needs to advance AI through these phases — it doesn't need full AI fidelity.

### Notable Enemies

See [STORY.md](STORY.md) for character details. AI notes:

- **Spirolo** — extremely fast; slowed by water. Water tiles modify movement speed constant.
- **Twin Dolls** — perfectly mirrored attack pattern. Dodging one leads into the other.
  Implemented as two AI agents sharing a mirrored state machine.
- **Amblico** — corrupts Clare's clairvoyance. His presence causes shims to show *false*
  positions. Implemented by injecting noise into the lookahead output, not the sim itself.
- **King Dev** — no magic; psychologically manipulates Clare's shims. Fakes wind-ups,
  speaks during combat. AI reads Clare's dodge history to find exploitable patterns.

---

## Script Layer (Scene Files)

Game content is defined in **JSON scene files** (`src/levels/*.json`), parsed by the game
layer into a `WorldState` plus a list of events. The engine is unchanged — it just renders
the `WorldState` and applies the `Diff`s the event system emits. JSON was chosen over an
embedded scripting language so the simulation stays trivially **deterministic** (data, not
executed code) and identical across desktop / WASM / Switch.

Implementation: `src/game/json.h` (parser), `src/game/scene.{h,cpp}` (loader),
`src/game/events.{h,cpp}` (runtime). Run a scene with the `CV_SCENE` env var (desktop) or
`?CV_SCENE=` query param (web).

```bash
make demo                          # runs src/levels/demo.json in a window
make demo SCENE=src/levels/foo.json   # run a different scene
```

In the demo, the player penguin walks toward Mochi; when it gets within range a proximity
event fires — printing `CV_DIALOGUE: mochi_intro`, flipping Mochi to a surprised frame, and
sending Mochi leaping away via `set_motion`.

### Scene schema

```json
{
  "version": 1,
  "sheets": [ { "path": "art/penguin.png", "cols": 16, "rows": 1 } ],
  "cameras": [
    { "projection": "perspective", "position": [6, 7, 16], "target": [6, 0, 5] }
  ],
  "activeCamera": 0,
  "archetypes": {
    "penguin": {
      "scale": [0.9, 0.9], "billboard": true, "sheet": 0,
      "clips": {
        "idle": { "first": 7, "count": 1, "fps": 0 },
        "walk": { "first": 0, "count": 5, "fps": 10 }
      }
    }
  },
  "entities": [
    { "id": "player", "archetype": "penguin", "pos": [0, 0.45, 5],
      "vel": [1.2, 0, 0], "anim": "walk" },
    { "id": "mochi",  "archetype": "penguin", "pos": [11, 0.45, 5], "anim": "idle" }
  ],
  "events": [ /* see below */ ]
}
```

`version` is the scene format version (currently 1). The loader warns on stderr when
it is missing and rejects versions newer than it supports. The loader is **strict**
(see the MAP_EDITOR spec): unknown trigger/action types, unknown condition ops,
duplicate entity ids, entity/archetype/clip references that don't resolve, and
malformed vectors (wrong length) are hard errors with contextual messages such as
`events[3].actions[0]: unknown action type 'set_flg'` — typos fail the load instead
of silently becoming defaults.

`sheets` lists every sprite sheet the scene uses (loaded into the renderer's texture
array in order — see ARCHITECTURE.md); the singular `"sheet"` object is still accepted
as `sheets[0]`. An entity's optional `"sheet"` field is an index into that list
(default 0).

Entities are referenced by string `id` in the data file; the loader assigns numeric
`EntityId`s (starting at 1) and keeps the name→id map. Optional fields: `vel`, `accel`
(motion, evaluated on the GPU and mirrored on the CPU for triggers), `sheet`,
`tint` (`[r, g, b, a]` multiplier — alpha < 1 renders in the translucent pass),
`rotation` and `pitch` (non-billboard orientation, radians — see ARCHITECTURE.md;
pitch −π/2 lies a quad flat as a floor tile).

### Archetypes and named animation clips

An **archetype** is a named entity template declared in the top-level `"archetypes"`
object. Its fields (`scale`, `billboard`, `sheet`, `speed`, ... — any entity field)
apply as *defaults* to every entity that references it via `"archetype"`; fields the
entity sets itself override the archetype's, key by key. An archetype's `"clips"`
map names animation ranges (`"walk"`, `"idle"`, `"windup"`) so authored data says
*what plays*, never raw frame numbers:

- an entity's `"anim"` may be a **string** — a clip name resolved through its
  archetype — or the raw inline `{ "first", "count", "fps" }` object (back-compat);
- the `set_anim` action may give `"clip": "surprised"` instead of `first/count/fps`
  (the clip is resolved through the *target entity's* archetype at runtime).

The loader keeps the parsed archetypes (name → clips) and the entity → archetype
mapping on the `Scene`; `demo.json` is the reference example. Raw frame indices
remain valid but are discouraged in authored content (MAP_EDITOR spec).

### Event / Condition / Action system

Each event is a **trigger** + an optional **condition** + a list of **actions**, evaluated
every simulation step. Triggers are pure functions of sim state (entity positions, flags) —
no wall-clock, no `rand()` — so they fire deterministically.

```json
{
  "trigger": { "type": "proximity", "entity": "player", "target": "mochi", "radius": 1.5 },
  "condition": { "flag": "mochi_seen", "value": false },
  "once": true,
  "actions": [
    { "type": "dialogue", "id": "mochi_intro" },
    { "type": "set_flag", "flag": "mochi_seen", "value": true },
    { "type": "set_anim", "entity": "mochi", "first": 6, "count": 1, "fps": 0 }
  ]
}
```

| Trigger     | Fields                          | Fires when |
|-------------|---------------------------------|------------|
| `start`     | —                               | first step (subject to condition) |
| `proximity` | `entity`, `target`, `radius`    | distance(entity, target) ≤ radius |
| `input`     | `action`, `edge`                | the abstract `action` shows `edge` this tick (`pressed`/`released`/`held`; default `pressed`) — see Controls / Input |

### Flags and conditions

Flags are **numeric** (`double`): booleans are just 0/1 — `"value": true` in data is
1.0 — and the same store holds counters (`keys`, `score`). A flag that was never set
reads as 0 (false). A condition is either a single flag comparison or an `all`/`any`
composition (recursive to any depth):

```json
"condition": { "all": [
  { "flag": "keys", "op": "ge", "value": 3 },
  { "any": [ { "flag": "door_open", "value": true },
             { "flag": "lives", "op": "ne", "value": 0 } ] }
] }
```

| Condition form | Fields | Passes when |
|----------------|--------|-------------|
| flag comparison | `flag`, `value`, `op` (optional) | `flags[flag] <op> value`; `op` is one of `eq` (default), `ne`, `lt`, `le`, `gt`, `ge` |
| `all` | array of conditions | every child passes (empty `all` passes) |
| `any` | array of conditions | at least one child passes (empty `any` fails) |

| Action       | Fields                                  | Effect |
|--------------|-----------------------------------------|--------|
| `dialogue`   | `id`                                    | emits a `CV_DIALOGUE: <id>` line (UI sink TBD) |
| `set_flag`   | `flag`, `value`                         | sets a flag (boolean or number) |
| `add_flag`   | `flag`, `value`                         | adds `value` to a flag (counters — a missing flag starts at 0) |
| `set_anim`   | `entity`, `clip` *or* `first`, `count`, `fps` | swaps an entity's animation (emits an upsert `Diff`); `clip` names an archetype clip |
| `set_motion` | `entity`, `vel`, `accel`                | rebases motion from the entity's current position and gives it a new velocity/acceleration (e.g. an enemy fleeing or a thrown arc) |
| `remove`     | `entity`                                | despawns an entity (emits a removal `Diff`) |
| `toggle_controlled` | `entity`                         | flips the entity's `controlled` attribute (adds/removes it from the controlled set) |
| `set_controlled`    | `entity`, `value`                | sets the entity's `controlled` attribute to `value` |

`once` (default true) fires the event at most once. Events run from the simulation step and
produce `Diff`s the renderer applies — they never call GL. This is the same seam the
clairvoyance lookahead uses, so shim ghosts and scripted events share one deterministic sim.

### Bitmap font / text entities

Text is not a special renderer feature — it's data. `src/game/text.{h,cpp}` provides
`makeText(s, pos, charW, charH, fontSheet, tint)`, a pure function that turns a string into
one non-billboard `Instance` per character, laid out along `+X`, each sampling a cell of a
font sheet keyed by `frame = ascii - 32`. The renderer never learns what "text" is; it just
draws the quads it's given, exactly like every other sprite.

`scripts/gen_font.py` generates `art/font.png`: 16 cols × 6 rows of 16×16 cells, white glyphs
on transparent, covering ASCII 32 ("space") through 127 — the same flat cell-index convention
as every other sheet.

A scene entity with a `"text"` field expands to N glyph entities instead of one:

```json
{ "id": "title", "text": "FIND 3 KEYS", "pos": [1, 2, 0],
  "charSize": [0.5, 0.5], "sheet": 2, "tint": [1, 1, 1, 1] }
```

`charSize` is the per-glyph `[width, height]` (default `[0.5, 0.5]`); `sheet` indexes the
scene's `"sheets"` list (a font sheet). The entity's `id` binds to its *first* glyph, so
triggers/actions can reference it by name as usual; `Scene::textRangeEnd` records the
`[first, last]` glyph id range so the `remove` action despawns every glyph as one unit
(`events.cpp`). There is no `set_text` action yet — static text plus `remove` covers the
current demos.

### Dialogue text (`dialogueText` scene config)

The `dialogue` action always emits its `CV_DIALOGUE: <id>` printf. A scene may additionally
declare a top-level `dialogueText` block:

```json
"dialogueText": { "pos": [0, 2.5, 4], "charSize": [0.3, 0.3], "sheet": 2, "tint": [1,1,1,1] }
```

When present, every `dialogue` action ALSO spawns its line id as glyph text at that position,
using ids from a reserved runtime range (`SimState::nextDynamicId`, starting at 1,000,000 —
well above any scene-authored id) so it can never collide with a named entity. The spawned
text despawns automatically after `DIALOGUE_TEXT_TICKS` (180 ticks = 3 seconds at the fixed
60 Hz tick) via `SimState::dialogueTextExpiry`, a tick-driven schedule checked once per
`stepSim` — deterministic, no wall clock, replay-safe like everything else in the sim.

---

## Controls / Input

Keyboard input is a small, device-agnostic layer (`src/game/input.{h,cpp}`) that sits between
the engine and the game logic. It has three pieces: an **InputFrame** (raw per-frame key state),
the **Bindings** (key → abstract action), and the **movement** pass that drives controlled
entities (`applyMovement`, declared in `scene.h`, run per tick by `stepSim`). The game layer is
**SDL-free**: the ENGINE builds the InputFrame from SDL events (`src/engine/sdl_input.{h,cpp}`,
`buildInputFrame`) once per rendered frame; the events demo path is unaffected (no bindings).

**Character/player movement is free — not grid-locked.** The 90° lock is a *camera* constraint
(see [CLAUDE.md](CLAUDE.md)), not a movement one.

### Abstract action model

The engine never hands SDL keycodes to game logic. Each frame it builds an `InputFrame` of
lowercase, device-agnostic key names — letters as-is (`"w"`), arrows as `"up"`/`"down"`/
`"left"`/`"right"`, space as `"space"` — split into three sets: keys currently `down`, keys
`pressed` this frame, and keys `released` this frame. Bindings then resolve those keys into
**abstract actions**, so game data talks about intent (`move_north`) rather than hardware.

### Bindings schema

A scene may declare a `controls` block mapping key names to abstract action names:

```json
"controls": {
  "bindings": {
    "w": "move_north", "a": "move_west", "s": "move_south", "d": "move_east",
    "up": "move_north", "left": "move_west", "down": "move_south", "right": "move_east",
    "space": "toggle_extra"
  }
}
```

Multiple keys may map to the same action (WASD *and* arrows above). Unbound keys contribute
nothing. The bindings are parsed onto the `Scene`; the active input source
(`BindingsInputSource`) holds a **copy** of them — so it stays valid across scene
unload/hot-reload — and resolves each tick's `InputFrame` into down/pressed/released
**action** sets via `ResolvedActions poll(const InputFrame&)`. Edges (pressed/released) are
consumed by the first sim tick of the frame and never repeat on later ticks; held state
persists. Resolution is a pure function — the per-tick input is effectively a tick-stamped
command, ready for lockstep replay/networking.

### Entity attributes: `controlled` and `speed`

Movement is driven by per-entity, game-side attributes — kept separate from the GPU `Instance`
(which must stay a standard-layout block of floats). They live in an `EntityAttrs` store inside
the copyable `SimState` (`src/game/sim.h`), initialised from the immutable scene:

| Field        | Default | Meaning |
|--------------|---------|---------|
| `controlled` | `false` | movement input applies to *every* entity whose `controlled` is true |
| `speed`      | `3.0`   | movement rate in world units / second |
| `radius`     | `0.3`   | collision circle around `pos` (see World → Collision) |
| `solid`      | `false` | stands in the way of controlled entities |
| `autoAnim`   | `false` | switch between the archetype's `walk` / `idle` clips when starting / stopping, and mirror the sprite to face its direction of travel |
| `patrol`     | —       | walk back and forth forever (see World → Patrols) |

All are optional entity JSON fields:

```json
{ "id": "player", "pos": [0, 0.9, 0], "controlled": true, "speed": 3.0, ... }
```

`controlled` is a per-entity attribute, **not** a global "who is active" flag — you can control
one entity, several at once, or toggle membership at runtime (via `toggle_controlled` /
`set_controlled`). This is what lets the controls demo move the player alone, then add a buddy.

### Movement model

The directional actions `move_north` (−Z), `move_south` (+Z), `move_west` (−X), `move_east` (+X)
follow the engine coordinate convention (+X east, +Z south, +Y up). In a scene with a camera
rig (see World → Camera rig) the four directions are **relative to the camera** instead:
"north" is away from the camera, whichever of the four ways it faces. Held directions sum into a
vector, normalised if non-zero (so diagonals aren't faster), then scaled by each
controlled entity's `speed` and applied as **velocity** — using the same rebase-to-current-
position + `motionStart = now` approach as `set_motion`, at the tick's sim time. An upsert is
emitted only when an entity's desired velocity actually differs from its current velocity, so a
stationary or steady-moving entity produces no per-tick `Diff` churn.

### `input` trigger

The `input` trigger fires when an abstract action shows a given edge this frame:

```json
{ "trigger": { "type": "input", "action": "toggle_extra", "edge": "pressed" }, "once": false,
  "actions": [ { "type": "toggle_controlled", "entity": "buddy" } ] }
```

`edge` is one of `pressed` / `released` / `held` (default `pressed`). The resolved action sets
are threaded into `updateEvents` via `stepSim` each tick; a scene without bindings (like the
events demo) simply runs with empty action sets.

### Demo

```bash
make demo-controls   # src/levels/controls.json
make demo-events     # src/levels/demo.json (the proximity/dialogue demo)
```

In `demo-controls`, WASD or the arrow keys move the player penguin freely. Pressing **space**
fires an `input` trigger that toggles a second "buddy" penguin into the controlled set, so both
move together; pressing space again drops the buddy back out.

---

## Demos

Every demo is a plain scene JSON under `src/levels/`, run via `make demo-<name>` (desktop) or
the matching card on the [landing page](https://clare.dobuki.net) (web). Each one isolates a
layer or feature so a regression is easy to localize:

| Demo | Make target | Scene | Proves |
|------|-------------|-------|--------|
| **Village** (flagship) | `make demo-village` | `village.json` | Talking and collecting: the dialogue window with the speaker's face, choices that set variables, lines filtered on variables and on the inventory, pickups, and a small quest that uses all of it. See "The village demo". |
| **Explore — top-down** | `make demo-explore` | `explore.json` | A world to walk around in, seen from above at 45°: terrain (authored map + procedural wilds), collision, the follow camera, labels, patrols with shims, sky. See "The explore demo". |
| **Explore — dungeon crawl** | `make demo-dungeon` | `explore.json` (`CV_VIEW=dungeon`) | The same world from the third-person dungeon view. |
| **Quest** | `make demo-quest` | `quest.json` | The event system as one small playable game: archetypes + named clips, flags/conditions gating a door, bitmap-font text, a patrolling enemy driven by `set_motion` + proximity, and a real shim preview (see below). |
| World | `make demo-world` | `world.json` | Tiled floor (pitched quads) + brick walls (upright quads) from a second sprite sheet, plus a translucent ghost — the two-pass opaque/translucent render path. |
| Controls | `make demo-controls` | `controls.json` | The `input` trigger and `toggle_controlled`/`set_controlled` — free WASD/arrow movement, toggling a second controlled entity. |
| Menu | `make demo-menu` | `menu.json` | Flags + `input` triggers driving pure animation-state UI (no 3D movement). |
| Events | `make demo-events` | `demo.json` | The original proximity → dialogue → `set_motion` chain (Mochi). |
| Stress Test | `make demo` (no `SCENE=`) / `stress.html` | — (code-built) | Raw throughput: 900 instanced billboards, GPU-driven animation, no scene/event system involved. |

### The village demo

`src/levels/village.json`: a crossroads village beside sealed ruins. The quest, start to
finish: the **elder** wants three moonherbs (say yes, ask what's in it for you, or put it
off — he remembers which); bring them and he hands over the ruins key. **Nina** sells a
lantern for two coins, which lie around the village. **Bram** guards the gate and wants to
see both key and lantern. Inside the ruins, a chest holds the lost crown — and the elder has
something to say about that. **Old Tuck** sleeps through all of it unless you are carrying a
lantern.

Controls: WASD / arrows walk, **SPACE** or ENTER talks and moves a conversation on,
up / down pick a choice, Q / E turn the camera, V switches view (it plays in both).

Nothing in it is special-cased in code: the whole quest is the `dialogues`, `items` and three
`events` in the scene file.

### The explore demo

`src/levels/explore.json`: a hand-drawn core — a walled ruin with rooms, corridors and a boss
hall; two roads that cross; a hedged village green; a pond with a plank bridge — set in
procedural wilds that go on forever in every direction (meadow, dunes with cacti, forest).
There is nothing to *do* yet; it is a place to walk around in and a test bed for both views.

Every character is the placeholder penguin, told apart by tint, size and the small label
under its feet: `HERO`, `ENEMY` (patrolling, each with a translucent shim one second ahead of
it), `BOSS`, `NPC`, `OBJECT` (things to pick up, later), `DECOR` (statues, barrels). Plants and
rocks are real terrain art (`art/terrain.png`), grown by the terrain generator.

Controls: WASD / arrows walk (relative to the camera), **Q / E** turn the camera 90°,
**V** or space switches between the top-down and dungeon views. Both demo cards load the same
scene; `CV_VIEW=topdown|dungeon` only picks the view it starts in.

### The quest demo

`src/levels/quest.json`: a small room (6×6 main area + a 6×2 vault behind a gated door). The
player (a controlled penguin archetype) must find 3 keys — each a proximity trigger that
`remove`s the key, `add_flag`s a `keys` counter, and fires a `dialogue`. The gate's middle
segment (`door`, tinted purple to read as distinct from the plain brick walls) only despawns
once a `condition: { "flag": "keys", "op": "ge", "value": 3 }` passes on a proximity trigger,
revealing a "WIN" bitmap-font text and a `win_marker` proximity zone that fires a `victory`
dialogue and swaps the player to its `chat` clip. A second penguin patrols back and forth
between two invisible marker entities (tint alpha 0, so they're discarded by the fragment
shader and never drawn) via a pair of mirrored proximity → `set_motion` events; touching it
fires an `ouch` dialogue and a small `set_motion` knockback (non-fatal — this engine has no
collision/health system yet, so it's flavor, not damage).

**The shim preview**, done with zero new engine code: `enemy_shim` is a second Instance with
the *exact same* starting `pos`/`vel` as `enemy`, but `"motionStart": -1.0` — one second
"pre-aged" on the same analytic motion formula (`pos + vel·dt + ½·accel·dt²`) every Instance
already evaluates. At any moment it therefore renders exactly where the real enemy *will be*
one second from now. The trick is keeping that phase offset exact across the enemy's direction
flips: `enemy_shim` gets its own copies of the two patrol proximity events (against the same
markers, same radius), so it independently reaches each marker and flips its own velocity —
always exactly one second before the real enemy does, since both move at identical speed along
identical geometry. The result is a genuine clairvoyance-style "ghost ahead" preview (tinted
`[0.5, 0.8, 1, 0.35]`, translucent pass), built entirely from `motionStart` + proximity triggers
+ `set_motion` — the same primitives every other demo uses. See `docs/specs/SHIM_SYSTEM.md` for
where this goes next (a real forward-simulation lookahead rather than a hand-authored mirror).

---

## World

Everything a scene needs to be a place rather than a stage: ground to stand on, walls to stop
at, a camera that follows, and a way to tell the penguins apart. All of it is optional —
scenes without these blocks behave exactly as before.

### Terrain — `"terrain"` (`src/game/terrain.h`)

The world is a grid of 1×1 cells. What a cell contains is a **pure function of its
coordinates**:

- inside the **authored map** — ASCII `rows`, one character per cell, `origin` at the
  top-left — the `legend` entry for that character decides;
- everywhere else, and on authored cells that don't name a floor, the cell is **generated**
  from `hash(seed, x, z)`.

```json
"terrain": {
  "sheet": 1, "seed": 174, "radius": 26, "origin": [0, 0],
  "legend": {
    "=": {"floor": 7},
    "#": {"wall": 9, "wallBase": 26, "top": 10, "height": 2},
    "~": {"floor": 13, "floorFrames": 2, "floorFps": 1.5, "solid": true},
    "_": {"props": false},
    "E": {"floor": 8, "spawn": ["enemy_ew", "enemy_ew_shim"]}
  },
  "rows": [ "#####", "#E..#", "##=##" ],
  "wild": { "scale": 12, "biomes": [ ... ] }
}
```

Legend fields (all optional; a character with no entry — and the space — is plain wild ground):

| Field | Meaning |
|-------|---------|
| `floor` | fixed floor frame. Absent → generated ground. |
| `floorFrames`, `floorFps` | animated floor (water) — shader animation, as for sprites |
| `wall`, `height` | a wall `height` cells tall: one quad per exposed face per storey |
| `wallBase` | frame for the ground storey (mossy foot); defaults to `wall` |
| `top` | frame for the wall's top; defaults to `wall` |
| `solid` | blocks movement without being a wall (water) |
| `props` | `false` keeps generated ground bare (a lawn, a patrol lane) |
| `spawn` | archetype name, or a list: one entity each, standing at the cell centre, named `<archetype>@<x>,<z>` |

Walls only emit the faces that can be seen (a face against an equally tall neighbour is
skipped) and are shaded by the direction they face — a fixed fake sun — so corners read as 3D
with no lighting in the shader.

**The wilds.** `wild.biomes` is a list of bands over a smooth noise value (0–255, lattice
spacing `scale` cells): each cell falls in the first band whose `upTo` exceeds its value.
A biome names its ground tiles (`floor`: one frame or several, picked by hash, rotated by a
random quarter turn so the tiling never shows) and what may grow there:

```json
{ "name": "dunes", "upTo": 88, "floor": [5, 5, 6], "props": [
    {"cell": 21, "perMille": 40, "scale": [1.1, 1.1], "solid": true, "radius": 0.25},
    {"cell": 25, "perMille": 45, "scale": [0.8, 0.8]} ] }
```

Each cell rolls once (0–999) against the props in order; a hit plants that prop, nudged off
the cell centre by a hashed offset. So cell (10, 7) has a cactus because
`hash(seed, 10, 7)` says so — walk away, come back, and it is the same cactus, with nothing
saved anywhere. Change the seed and the whole world is different.

Generation is **integer math only** (hashes, fixed-point smoothstep; plant offsets are exact
multiples of 1/64), so every platform grows the identical world.

**Streaming.** Terrain tiles are *not* entities and never enter the `SimState`.
`TerrainStreamer` keeps the cells within `radius` of the followed entity instanced in the
renderer: when the hero crosses into a new cell it emits upserts for the cells that came into
range and removals for those left behind (a small hysteresis band avoids churn), and does
nothing at all otherwise. It is render-side bookkeeping with no gameplay truth in it —
collision does not ask what is loaded, it asks the function.

### Collision

Controlled entities are stopped by solid cells (walls, water), solid plants, and `solid`
entities. Each tick, per controlled entity, the step is tried one axis at a time against a
circle of the entity's `radius`, and the axis that would end inside something is cancelled —
which is what makes the entity slide along a wall instead of sticking to it. Because every
query goes through `Terrain::describe`, what blocks the hero depends only on where the hero
is: deterministic, and identical for a lookahead fork of the sim.

(The build uses `-ffp-contract=off` so arm64 and WASM compute the same floats — no fused
multiply-add on one side only.)

### Camera rig — `"cameraRig"`

```json
"cameraRig": {
  "follow": "hero@25,29", "view": "topdown", "facing": "north",
  "topdown": {"back": 5.6, "height": 6.1, "lookAhead": 0, "targetHeight": 0.5, "fovY": 0.8727},
  "dungeon": {"back": 2.7, "height": 1.35, "lookAhead": 2.5, "targetHeight": 0.75, "fovY": 1.2217}
}
```

`follow` defaults to the first controlled entity. The rig's math lives in the engine
(`src/engine/camera_rig.h` — see ARCHITECTURE.md, "The follow-camera rig"); what the game
layer owns is its **state**: `SimState::rig` holds the facing (in quarter turns) and the view,
changed by three abstract actions a scene binds keys to:

| Action | Effect |
|--------|--------|
| `camera_left` / `camera_right` | turn the camera 90° (eased; turns chain) |
| `camera_view` | switch between the top-down and dungeon views (eased blend) |

Movement is relative to the rig's facing, so after a turn "forward" is still away from the
camera.

### Labels — `"label"`

```json
"labelStyle": {"sheet": 2, "height": 0.22, "advance": 0.115, "drop": 0.02},
"archetypes": { "enemy": { "label": "ENEMY", "labelTint": [1, 0.45, 0.4, 1], ... } }
```

An entity (or its archetype) with a `label` gets small billboarded glyphs centred under its
feet. They are ordinary instances that share the owner's position and differ only in their
pivot `offset`; whenever the owner's motion changes the sim copies the same motion formula
onto them (`emitEntity`), so the GPU moves owner and label as one. Removing the entity removes
its label. Labels are "grounded" billboards, which is what keeps text hung below the feet from
being buried in the floor (see ARCHITECTURE.md).

Give the entity a pivot (`"offset": [0, 0.32]` for a 0.9-tall penguin) so that `pos` is where
its feet touch the ground — the label, the collision circle and the camera all work from that
point.

### Patrols — `"patrol"`

```json
"patrol": {"delta": [4, 0, 0], "speed": 1.3, "phase": 1.0}
```

Walk from `pos` to `pos + delta` and back, forever. A patrol is driven by the tick counter
alone — a leg lasts a whole number of ticks, and `patrolPose(tick)` says where it is — so it
needs no events and replays exactly. `phase` (seconds) runs the patrol that far ahead: give a
translucent twin the same patrol with `"phase": 1.0` and it is, at every instant, where the
original will be one second from now. That is the explore demo's enemy shims — a real
lookahead, for the price of one field. (The general forward-simulation lookahead is still to
come: `docs/specs/SHIM_SYSTEM.md`.)

### Talking — `"dialogues"` (`src/game/dialogue.h`)

A conversation is shown the way old RPGs did it: a window along the bottom of the screen,
the speaker's face in a badge at its left with their name above, the line typed out a few
letters at a time. The `interact` action finishes the line, then moves on. While the window
is open the player does not move and the camera does not turn; the world carries on.

A dialogue is a **flat list of lines, read top to bottom**:

```json
"dialogues": {
  "elder": [
    {"if": {"flag": "herb_quest", "value": 2}, "speaker": "elder", "end": true,
     "text": "The ruins are north, past Bram."},

    {"if": {"all": [{"flag": "herb_quest"}, {"item": "herb", "op": "ge", "value": 3}]},
     "speaker": "elder", "text": "Three moonherbs! My knees thank you already.",
     "take": {"herb": 3}, "give": {"key": 1}, "set": {"herb_quest": 2}, "end": true},

    {"speaker": "elder", "text": "Would you gather three moonherbs for me?",
     "choices": [
       {"text": "Of course.",            "set": {"herb_quest": 1}},
       {"text": "What is in it for me?", "set": {"herb_quest": 1, "greedy": 1}},
       {"text": "Not now.",              "set": {"elder_later": 1}} ]},
    {"if": {"flag": "elder_later"}, "speaker": "elder", "end": true,
     "set": {"elder_later": 0}, "text": "No matter. I am not going anywhere."},
    {"if": {"flag": "greedy"}, "speaker": "elder", "end": true,
     "text": "Hmph. Bring them and you will see."},
    {"speaker": "elder", "text": "Bless you. They grow in the meadows."}
  ]
}
```

| Line field | Meaning |
|------------|---------|
| `text` | what is said (required). Wrapped to the window; `\n` forces a break. |
| `speaker` | an **archetype** name: its sprite is the face, its `name` the name plate. Omitted → narration, no badge. |
| `if` | a filter — the same condition syntax as events. A line whose filter fails is **skipped**. |
| `choices` | `[{"text", "set"}]`: shown under the text once it is typed out; picking one sets variables. |
| `set` | variables set when the line comes up (`{"name": number-or-bool}`) |
| `give` / `take` | items handed to / taken from the player when the line comes up |
| `end` | close the conversation after this line |

That is all there is, and it is enough for a dialogue *tree*: a choice sets a variable, later
lines filter on it, so the list is walked along a different path each time. Filters are
checked as each line is reached — after everything the previous lines set, gave and took —
and variables persist, so the second visit to an NPC naturally differs from the first. When
the last line that passes has been read (or a line says `end`), the window closes. A dialogue
with no passing line does not open at all.

**Conditions** (shared with events) gained the inventory:

| Form | Passes when |
|------|-------------|
| `{"flag": "x"}` / `{"flag": "x", "op": "ge", "value": 2}` | the variable compares true (unset reads 0) |
| `{"item": "herb", "op": "ge", "value": 3}` | the item count compares true (never picked up counts 0) |
| `{"has": "key"}` / `{"lacks": "key"}` | at least one / none |
| `{"all": [...]}` / `{"any": [...]}` | every / any child passes |

**Who talks.** An entity (or its archetype) with `"talk": "<dialogue id>"` can be talked to:
standing within reach shows a hint ("SPACE: Talk to Elder Pim"), and `interact` opens its
dialogue. The nearest one wins; ties go to the lower id. Its `"name"` is what the hint and
the name plate show. While it talks, an NPC plays its `chat` clip.

**Events** can join in: the `talk` action opens a dialogue (an intro at `start`, a remark
when something is found), `toast` flashes a short message, `give` changes the inventory, and
the new **`always`** trigger fires as soon as its condition holds — which is how the village
gate opens the moment the guard sets `gate_open`.

Timing is ticks (the typewriter reveals 40 glyphs a second), so a conversation replays
exactly from the same key presses.

### Objects — `"pickup"` and the inventory

```json
"items": {"herb": "Moonherb", "coin": "Coin"},
"archetypes": { "herb": {"pickup": "herb", "label": "OBJECT", ...},
                "purse": {"pickup": {"item": "coin", "count": 3}, ...} }
```

An entity with a `pickup` is collected by walking into it: it leaves the world (label and
all) and its item count goes up. The inventory is just `item id → count` in the `SimState`;
the HUD lists it in the top-left corner and a toast announces each gain. `items` gives ids a
display name. Dialogue lines and events read it through conditions and change it with
`give` / `take`.

### The screen overlay — `"ui"`

```json
"sheets": [ ..., {"path": "art/font.png", "cols": 16, "rows": 6},
                 {"path": "art/ui.png",   "cols": 8,  "rows": 2} ],
"ui": {"sheet": 3, "font": 2}
```

Names the sheets the dialogue window, HUD and their text are drawn from (required for
`dialogues`). Everything on screen is built by `dialogue.cpp` as overlay instances —
see ARCHITECTURE.md, "The screen overlay". The sim remembers the ids it put up, by group
(`dialogue.box`, `hud.inventory`, ...), and replaces a group only when what it shows changes.

Overlay text is proportional: `scripts/gen_font.py` records each glyph's width in
`src/game/font_metrics.h`, and `makeTextBlock` spaces and word-wraps with it.

### Outdoors

- Long linear roads with forks. Forks lead to distinct areas; the primary path is always
  forward.
- No minimap. Navigation by landmark.
- Road segments are loaded from data files; the spatial cache (engine) loads/unloads tiles
  as the player moves.

### Dungeons

- More complex layouts. Third-person camera maintained throughout.
- Large open rooms may trigger overhead/isometric camera view (automatic or manual — TBD).
- Camera switch is a `Diff` that updates `activeCamera` index.

---

## Coordinate Conventions

Follows the engine's right-handed coordinate system:

- **+X** east, **+Z** south, **+Y** up
- Grid cells are 1×1 world units
- Character/player movement is free (not grid-locked); camera lerps smoothly between positions
- Camera rotation is locked to 90° increments (0°, 90°, 180°, 270°)

---

## Doc Sync Rule

Any change to `Instance`, `WorldState`, `Diff`, or `Camera` in the engine must update
[ARCHITECTURE.md](ARCHITECTURE.md) first. This doc references those contracts — it does
not duplicate field definitions. If the two docs contradict each other, ARCHITECTURE.md
is authoritative.
