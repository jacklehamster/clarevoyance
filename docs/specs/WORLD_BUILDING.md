# Spec: World Building

Environment/tile layer for outdoor roads and dungeons: floors, walls, collision, and
the spatial tile cache.

Engine contracts are defined in [ARCHITECTURE.md](../ARCHITECTURE.md); world-structure
intent (roads, forks, dungeons) in [GAME_LAYER.md](../GAME_LAYER.md#world-structure)
and [CLAUDE.md](../../CLAUDE.md).

---

## Purpose

Scenes currently contain only character sprites floating over the clear color. The
game needs walkable ground, walls that block movement, and landmarks — long linear
roads with forks outdoors, denser layouts in dungeons — defined in data, rendered by
the existing instanced sprite path wherever possible.

---

## Requirements

### Geometry: floors and walls

1. **MUST** support floor quads (lying flat in the XZ plane) and wall quads (vertical,
   facing an arbitrary yaw). **Decided (owner): v1 environment geometry is textured
   grid quads only** — no meshes, heightmaps, or slopes; anything beyond flat textured
   quads on the grid is out of scope for v1.
2. **Blocker — orientation:** `Instance.rotation` is yaw-only; a quad cannot pitch to
   lie flat, so floor tiles are impossible today. The engine **MUST** gain either a
   pitch/orientation field on `Instance` (preferred; per the doc-sync rule,
   ARCHITECTURE.md is updated first) or a separate mesh path for environment geometry.
3. **MUST** render environment quads through the instanced draw path — one tile = one
   oriented (non-billboard) instance. No per-tile draw calls.
4. **SHOULD** keep environment tiles static (zero `vel`/`accel`) so the instance
   buffer only changes when the cache loads/unloads a region.

### Scene JSON: tile section

5. **MUST** add a `tiles` (or `geometry`) section to scene JSON, separate from
   `entities`, e.g.:
   `{ "sheet": "env", "cell": 3, "kind": "floor", "pos": [x, y, z] }` and
   `{ "kind": "wall", "pos": [...], "facing": 90, "solid": true }`.
6. **SHOULD** support compact region shorthand (e.g. a rect of floor cells) so a
   50-unit road is not 50 hand-written objects; the loader expands shorthand into
   individual tiles.
7. **MUST** validate tile data under the strict loader (MAP_EDITOR spec).

### Multi-sheet support (dependency)

8. **MUST** support more than one sprite sheet per scene (at minimum: one character
   sheet + one environment sheet). Options: texture array, atlas packing, or one draw
   call per sheet. Requires a resource vocabulary in `Diff`/`WorldState`
   (ARCHITECTURE.md change) — currently the renderer accepts exactly one texture fixed
   at init.

### Collision

9. **MUST** attach collision data to tiles: `solid` walls contribute AABBs; movement
   of controlled entities clamps against them in the sim step.
10. **MUST** keep collision fully deterministic and part of the fixed-timestep sim
    (shared requirement with SHIM_SYSTEM — the lookahead needs wall collision for
    enemy paths even though it skips player collision).
11. **SHOULD** support non-solid gameplay surfaces (e.g. water tiles that modify
    movement speed — Spirolo's weakness) as tile attributes read by the sim.

### Spatial tile cache

12. **MUST** implement a spatial cache that loads tiles within a radius of the player
    and discards distant ones, emitting upsert/removal `Diff`s.
13. **MUST** stay simple: fixed-size square chunks on a grid, load the 3×3 (or N×N)
    chunk neighborhood around the player, unload everything else. No prediction, no
    priority queues — complexity here caused problems in the prior prototype
    (CLAUDE.md).
14. **MUST** be deterministic in its *sim-visible* effects: which tiles are collidable
    depends only on player position, never on load timing. (Rendering may lag; physics
    may not — the lookahead sim must see the same walls every run.)

### World structure and chunking

15. **MUST** express outdoor areas as long linear roads with forks — forks lead to
    distinct areas via `change_scene` (SCENE_COORDINATOR) or in-scene branches; the
    primary path is always forward.
16. **MUST** support large worlds by splitting tile data into chunk files (or chunk
    sections within a scene file) the cache streams in; a scene references its chunk
    set rather than inlining every tile. Format details deferred to MAP_EDITOR schema.
17. **MAY** later support generated chunks (data-defined procedural roads) behind the
    same chunk interface, provided generation is seeded and deterministic.

### Navigation (hard constraints)

18. **MUST NOT** add a minimap, ever. Navigation is by landmark and memory.
19. **MUST** make landmarks a first-class tile/entity role: distinctive, memorable
    props placed at forks and decision points are the navigation system.
20. **MUST** preserve deliberate pacing: long uneventful road stretches before
    encounters are intentional; tooling should not "optimize" them away.

---

## Non-goals

- Full 3D meshes, heightmaps, or slopes — flat textured grid quads only for v1
  (owner decision).
- Lighting/shadows.
- Dungeon overhead-camera trigger implementation (camera behavior lives in the engine
  camera system — the tile layer only needs to tag "large open room" regions).
  **Decided (owner): the overhead camera engages automatically based on room size,
  with a manual player override.**
- The map editor itself (MAP_EDITOR spec).

---

## Open questions

- Instance orientation encoding: full pitch angle, an axis enum (`floor`/`wall_x`/
  `wall_z`/`billboard`), or quaternion? Axis enum is likely sufficient and cheapest.
- Chunk size in world units (16×16? 32×32?) — tune against instance-buffer upload cost.
- Are chunks separate files or sections of one scene file? (Interacts with the WASM
  preload bundle — see MAP_EDITOR canonical-directory requirement.)

---

## Dependencies

- **Engine:** orientation fix (req 2) and multi-sheet/resource vocabulary (req 8) —
  both are `Instance`/`Diff` contract changes; ARCHITECTURE.md must be updated first.
- **SHIM_SYSTEM:** fixed-timestep deterministic sim (collision lives inside it).
- **SCENE_COORDINATOR:** forks that lead to different areas use `change_scene`.
- **MAP_EDITOR:** strict loader, schema versioning, and chunk file format.
