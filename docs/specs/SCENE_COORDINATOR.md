# Spec: Scene Coordinator

Multi-scene lifecycle — loading, unloading, and swapping scenes at runtime without a
process restart, plus the state that persists across scenes.

Engine contracts (`Instance`, `WorldState`, `Diff`, `Camera`) are defined in
[ARCHITECTURE.md](../ARCHITECTURE.md) and are referenced, not restated, here.

---

## Purpose

Today the process runs exactly one scene, chosen at startup via `CV_SCENE`. The game
needs to move Clare between roads, dungeons, and boss arenas within a single session,
carrying her progression with her. The Scene Coordinator owns which scene is active,
how transitions happen, and what state survives a swap.

---

## Requirements

### Lifecycle

1. **MUST** support loading a new scene, tearing down the current one, and activating
   the new one at runtime, without restarting the process, on desktop and WASM.
2. **MUST** rebuild or re-point all per-scene resources on swap: the renderer's sprite
   sheet texture, the event system, entity attribute store, and the input source.
3. **MUST** reset the renderer via a full `applyState(WorldState)` on scene activation —
   no stale instances from the previous scene may survive.
4. **SHOULD** keep the previous scene renderable until the new scene has finished
   loading (or show a transition), so a failed load never leaves a black screen with no
   recovery path.
5. **MAY** support preloading the next scene in the background (useful for road forks).

### `change_scene` action and spawn points

6. **MUST** add a `change_scene` action to the event system:
   `{ "type": "change_scene", "scene": "<scene name>", "entry": "<spawn point name>" }`.
7. **MUST** support named entry/spawn points in scene JSON, e.g.
   `"spawns": { "from_road": { "pos": [0, 0.9, 5], "facing": 180 } }`. `change_scene`
   places the player entity at the named spawn; a scene **MUST** declare a `default`
   spawn used when `entry` is omitted.
8. **MUST** resolve scene names through a defined root (see MAP_EDITOR spec, asset
   paths) — actions reference scenes by name, not by filesystem path.

### Persistent vs scene-local state

9. **MUST** split game state into two stores:
   - **GameState (persistent):** cross-scene flags and counters, inventory, Clare's
     clairvoyance upgrade tier. Survives scene swaps; serializable for save games.
   - **Scene state (local):** entity positions, event `fired` markers, scene-local
     flags. Discarded on unload.
10. **MUST** adopt the namespacing convention: flag/counter names beginning with
    `global.` live in GameState; all others are scene-local. Triggers and actions
    (`set_flag`, conditions) resolve names through this rule transparently.
11. **SHOULD** extend flags beyond booleans to integer counters with comparison
    conditions (`==`, `>=`, `<`) — required for inventory checks and upgrade gating.
    (Shared requirement with the event system; see Open questions.)

### Time

12. **MUST** give each scene its own sim-time epoch: sim time restarts at 0 (tick 0)
    on scene activation. All `motionStart` / `animStart` values in a scene are relative
    to that epoch, so scene files are position-independent in time.
13. **MUST NOT** carry wall-clock time into the sim; the fixed-timestep sim (see
    SHIM_SYSTEM spec) is the only time source the coordinator advances.

### Transitions

14. **SHOULD** provide a fade-out/fade-in transition hook implemented with existing
    engine seams (e.g. clear-color ramp or a full-screen tinted quad driven by `Diff`) —
    not a new UI layer.
15. **MAY** support per-transition style data in the `change_scene` action (fade
    duration, cut).

---

## Current blockers (prerequisite refactors)

These are ground-truth findings from architecture review; each **MUST** be fixed before
runtime scene switching can work:

- **Renderer texture fixed at init** — `Renderer::init` binds one sprite sheet for the
  process lifetime. Needs a texture (re)load path, or multi-sheet support
  (WORLD_BUILDING spec).
- **Runtime state lives inside `Scene`** — `Event::fired` mutates the loaded scene, so
  scenes cannot be reloaded or shared. Runtime state must move to a separate
  `SceneRuntime` (also required by SHIM_SYSTEM lookahead forking).
- **`KeyboardInputSource` holds a reference into the Scene's bindings** — dangling on
  unload. Input must copy or re-bind on scene swap.

---

## Non-goals

- Save/load to disk (GameState must be *serializable*, but persistence format is a
  later spec).
- Seamless open-world streaming — that is chunked tile caching (WORLD_BUILDING), not
  scene swapping. Scene swaps are discrete, player-visible transitions.
- Multiple simultaneously-active scenes.

---

## Open questions

- Does the player entity definition travel with GameState (one canonical Clare), or
  does each scene declare its own player entity that the spawn point configures?
- Are counters/comparisons specified here or in a dedicated event-system spec? (Listed
  as SHOULD above; needed by both this spec and MAP_EDITOR archetypes.)
- Preload policy: always preload fork destinations, or load on demand with a fade?

---

## Dependencies

- **SHIM_SYSTEM** — fixed-timestep sim and Scene/runtime-state separation are shared
  prerequisites.
- **WORLD_BUILDING** — multi-sheet texture support removes the single-texture blocker.
- **MAP_EDITOR** — strict loader and canonical scene directory define how `change_scene`
  resolves scene names; schema version gates format evolution (spawns, `change_scene`).
