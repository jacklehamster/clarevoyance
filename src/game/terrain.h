// terrain.h — the ground the world stands on: authored tiles + endless wilds.
//
// The world is a grid of 1×1 cells; cell (x, z) spans [x, x+1) × [z, z+1).
// What a cell contains is a PURE FUNCTION of its coordinates:
//
//   - inside the authored map (the scene's ASCII "rows"), the legend entry for
//     that character decides: a fixed floor tile, a wall, water, ... or "wild";
//   - everywhere else — and on authored cells left wild — the cell is generated
//     procedurally from hash(seed, x, z): which biome it belongs to, which
//     ground tile it shows, whether a plant grows there and which one.
//
// Nothing is ever saved. Walk away, come back, and cell (10, 7) regenerates the
// same cactus, because the cactus IS the hash of (seed, 10, 7). That gives an
// infinite world for the cost of one function, and it keeps the simulation
// deterministic: collision asks the same function, so what blocks the hero
// depends only on where the hero is — never on what happens to be loaded.
//
// All generation is integer arithmetic (hashes and fixed-point interpolation),
// so desktop, WASM and Switch agree on every cell bit-for-bit.
//
// TerrainStreamer is the render-side companion: it keeps the cells around a
// focus point instanced in the renderer and drops the ones left behind. It is
// deliberately NOT part of the SimState — it holds no gameplay truth.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "world_state.h"

namespace cv {

struct JsonValue;

// Terrain instances take ids from this base upward — far above scene-authored
// ids (1..) and runtime dialogue glyphs (1'000'000..), so they never collide.
constexpr EntityId TERRAIN_ID_BASE = 2'000'000;

// One plant/rock a biome may grow on a cell.
struct PropRule {
    int   cell = 0;          // frame in the terrain sheet
    int   perMille = 0;      // chance per cell, in 1/1000
    Vec2  scale = {1, 1};    // billboard size in world units
    bool  solid = false;     // blocks movement (a circle of `radius`)
    float radius = 0.3f;
};

// A band of the biome noise: every cell whose noise value is below `upTo`
// (and not claimed by an earlier band) belongs to this biome.
struct Biome {
    std::string name;
    int upTo = 256;                 // exclusive upper bound of noise value (0..255)
    std::vector<int> floors;        // ground tile variants (frames), picked by hash
    std::vector<PropRule> props;    // tried in order; first hit wins
};

// What one legend character of the authored map means.
struct TileKind {
    bool wild = true;        // no fixed floor → generated ground (and plants)
    int  floor = -1;         // fixed floor frame (wild == false)
    int  floorCount = 1;     // animated floor: frame count ...
    float floorFps = 0.0f;   // ... and speed (water ripples)
    int  wall = -1;          // side-face frame; -1 = no wall
    int  wallBase = -1;      // side-face frame of the ground storey (defaults to `wall`)
    int  top = -1;           // wall top frame (defaults to `wall`)
    int  height = 0;         // wall height in whole cells
    bool solid = false;      // blocks movement even without a wall (water)
    bool props = true;       // wild cells only: allow plants to grow here
    std::vector<std::string> spawn;  // archetypes to spawn one entity each from, here
};

// Everything there is to know about one cell.
struct CellDesc {
    bool  exists = false;    // false = void (no floor at all)
    int   floor = 0;
    int   floorCount = 1;
    float floorFps = 0.0f;
    int   floorTurn = 0;     // quarter turns of the floor quad (breaks up tiling)
    float shade = 1.0f;      // brightness multiplier (subtle per-cell variation)
    int   wall = -1;
    int   wallBase = -1;
    int   top = -1;
    int   height = 0;
    bool  solid = false;     // wall or water: the whole cell blocks movement

    bool  hasProp = false;
    int   propCell = 0;
    Vec2  propScale = {1, 1};
    float propX = 0.0f;      // world position of the prop's foot
    float propZ = 0.0f;
    bool  propFlip = false;
    bool  propSolid = false;
    float propRadius = 0.3f;
};

// An entity the authored map asks for ("spawn" legend entries).
struct TerrainSpawn {
    std::string archetype;
    int x = 0;
    int z = 0;
};

struct Terrain {
    bool enabled = false;
    int  sheet = 0;              // sprite-sheet index of the terrain tiles
    uint32_t seed = 1;
    int  radius = 22;            // cells kept instanced around the focus

    // Authored map: `width` × `height` cells with (originX, originZ) at the
    // top-left; `cells` holds an index into `kinds` per cell, row-major.
    int originX = 0, originZ = 0;
    int width = 0, height = 0;
    std::vector<uint8_t> cells;
    std::vector<TileKind> kinds;

    // Procedural wilds. Disabled → cells outside the authored map are void.
    bool wild = false;
    int  biomeScale = 12;        // biome noise lattice spacing, in cells
    std::vector<Biome> biomes;

    // The authored kind at a cell, or nullptr outside the map.
    const TileKind* kindAt(int x, int z) const;

    // The pure function everything else is built on.
    CellDesc describe(int x, int z) const;

    // Wall height at a cell, in cells (0 = no wall).
    int wallHeightAt(int x, int z) const;

    // True if a circle of radius r centred at (x, z) overlaps anything solid:
    // a wall/water cell, or a solid plant.
    bool blocksCircle(float x, float z, float r) const;

    // How much of the straight line from → to is free of walls, as a fraction
    // in [0, 1] (1 = fully clear). Used to pull the camera in front of a wall.
    float sightClearance(Vec3 from, Vec3 to) const;

    // Append the renderable quads of one cell: floor, wall faces, top, plant.
    void buildCell(int x, int z, std::vector<Instance>& out) const;
};

// Parse a scene's "terrain" block. Legend entries carrying "spawn" produce one
// TerrainSpawn per matching map cell. Returns false and fills `error` (with a
// "terrain..." context path) on malformed data.
bool readTerrain(const JsonValue& t, Terrain& out,
                 std::vector<TerrainSpawn>& spawns, std::string& error);

// Keeps the cells within Terrain::radius of a focus point instanced in the
// renderer. Call update() whenever the focus moves; it emits upserts for cells
// that came into range and removals for cells left behind, and does nothing at
// all while the focus stays inside the same cell.
class TerrainStreamer {
public:
    void reset(Diff& out);   // drop everything (scene reload)
    void update(const Terrain& terrain, Vec3 focus, Diff& out);

    size_t loadedCells() const { return loaded_.size(); }

private:
    static uint64_t key(int x, int z) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
               static_cast<uint32_t>(z);
    }

    std::unordered_map<uint64_t, std::vector<EntityId>> loaded_;
    std::vector<EntityId> freeIds_;
    EntityId nextId_ = TERRAIN_ID_BASE;
    bool hasFocus_ = false;
    int focusX_ = 0, focusZ_ = 0;
    std::vector<Instance> scratch_;
};

} // namespace cv
