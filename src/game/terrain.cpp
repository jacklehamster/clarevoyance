// terrain.cpp — see terrain.h.
#include "terrain.h"
#include "json.h"

#include <cmath>
#include <cstdio>

namespace cv {

namespace {

constexpr float HALF_PI = 1.5707963f;

// Independent hash streams per purpose, so e.g. the plant roll of a cell is
// uncorrelated with its ground tile.
constexpr uint32_t SALT_BIOME  = 1;
constexpr uint32_t SALT_DITHER = 2;
constexpr uint32_t SALT_GROUND = 3;
constexpr uint32_t SALT_PROP   = 4;
constexpr uint32_t SALT_PLACE  = 5;

// Biome borders are roughened by nudging each cell's noise value by up to
// this much either way, so biomes interlock instead of meeting on a clean line.
constexpr int BIOME_DITHER = 9;

// A plant stands within this distance of its cell centre, in 1/64ths of a
// cell (exactly representable, so the position needs no rounding at all).
constexpr int   PROP_JITTER_STEPS = 12;
constexpr float PROP_JITTER_UNIT  = 1.0f / 64.0f;

// How far a solid plant can reach out of its own cell: jitter + largest radius
// we accept from data. Collision checks widen their cell range by this much.
constexpr float PROP_REACH = 0.75f;

// Wall faces are shaded by the direction they face — a fixed fake sun from the
// south-east — so corners read as 3D without any lighting in the shader.
constexpr float SHADE_SOUTH = 1.00f;
constexpr float SHADE_EAST  = 0.86f;
constexpr float SHADE_WEST  = 0.76f;
constexpr float SHADE_NORTH = 0.64f;
constexpr float SHADE_TOP   = 0.92f;

// Each cell is darkened by a random amount down to this factor — just enough
// to keep large flat areas from looking printed, not enough to show the grid.
constexpr float SHADE_MIN = 0.965f;

// Camera sight test: sample spacing along the line, and how far in front of a
// wall the clear stretch is cut short (keeps the near plane out of the wall).
constexpr float SIGHT_STEP   = 0.08f;
constexpr float SIGHT_MARGIN = 0.30f;

// Bob Jenkins-style avalanche over (seed, x, z, salt). Integer only.
uint32_t hash32(uint32_t seed, int32_t x, int32_t z, uint32_t salt) {
    uint32_t h = seed ^ (salt * 0x9E3779B9u);
    h ^= static_cast<uint32_t>(x) * 0x85EBCA6Bu;
    h = (h << 13) | (h >> 19);
    h *= 0xC2B2AE35u;
    h ^= static_cast<uint32_t>(z) * 0x27D4EB2Fu;
    h = (h << 15) | (h >> 17);
    h *= 0x165667B1u;
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

// Floor division (rounds toward -inf), so negative coordinates tile correctly.
int floorDiv(int a, int b) {
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

int cellOf(float v) { return static_cast<int>(std::floor(v)); }

// Smooth biome noise in [0, 255]: random values on a coarse lattice, blended
// with a fixed-point smoothstep. Pure integer math.
int biomeNoise(uint32_t seed, int x, int z, int scale) {
    const int gx = floorDiv(x, scale), gz = floorDiv(z, scale);
    const int fx = x - gx * scale,     fz = z - gz * scale;
    auto lattice = [&](int ix, int iz) {
        return static_cast<int>(hash32(seed, ix, iz, SALT_BIOME) & 255u);
    };
    // Smoothstep weight in 0..256.
    auto weight = [&](int f) {
        return (256 * f * f * (3 * scale - 2 * f)) / (scale * scale * scale);
    };
    const int wx = weight(fx), wz = weight(fz);
    const int top = lattice(gx, gz)     * (256 - wx) + lattice(gx + 1, gz)     * wx;
    const int bot = lattice(gx, gz + 1) * (256 - wx) + lattice(gx + 1, gz + 1) * wx;
    return (top * (256 - wz) + bot * wz) >> 16;
}

Instance floorQuad(int x, int z, const CellDesc& d, int sheet) {
    Instance q = makeQuad({x + 0.5f, 0.0f, z + 0.5f}, {1, 1},
                          d.floorTurn * HALF_PI, -HALF_PI);
    q.sheet = static_cast<float>(sheet);
    q.tint = {d.shade, d.shade, d.shade, 1.0f};
    setAnimation(q, d.floor, d.floorCount, d.floorFps, 0.0f);
    return q;
}

bool fail(std::string& error, const std::string& ctx, const std::string& msg) {
    error = ctx + ": " + msg;
    return false;
}

} // namespace

const TileKind* Terrain::kindAt(int x, int z) const {
    const int cx = x - originX, cz = z - originZ;
    if (cx < 0 || cz < 0 || cx >= width || cz >= height) return nullptr;
    return &kinds[cells[static_cast<size_t>(cz) * width + cx]];
}

int Terrain::wallHeightAt(int x, int z) const {
    const TileKind* k = kindAt(x, z);
    return (k && k->wall >= 0) ? k->height : 0;
}

CellDesc Terrain::describe(int x, int z) const {
    CellDesc d;
    const TileKind* k = kindAt(x, z);
    if (!k && !wild) return d;   // outside the map, no wilds: void
    d.exists = true;

    const uint32_t hg = hash32(seed, x, z, SALT_GROUND);
    d.shade = SHADE_MIN + static_cast<float>(hg & 15u) * ((1.0f - SHADE_MIN) / 15.0f);

    if (k) {
        if (k->wall >= 0 && k->height > 0) {
            d.wall = k->wall;
            d.wallBase = k->wallBase >= 0 ? k->wallBase : k->wall;
            d.top = k->top >= 0 ? k->top : k->wall;
            d.height = k->height;
        }
        d.solid = k->solid || d.height > 0;
    }

    const bool generated = k ? k->wild : true;
    if (!generated) {
        d.floor = k->floor;
        d.floorCount = k->floorCount;
        d.floorFps = k->floorFps;
        return d;
    }
    if (biomes.empty()) { d.exists = (k != nullptr); return d; }

    // Which biome: smooth noise plus a per-cell nudge, matched against the
    // biome bands in order.
    int value = biomeNoise(seed, x, z, biomeScale > 0 ? biomeScale : 1);
    value += static_cast<int>(hash32(seed, x, z, SALT_DITHER) % (2 * BIOME_DITHER + 1))
             - BIOME_DITHER;
    const Biome* biome = &biomes.back();
    for (const Biome& b : biomes) {
        if (value < b.upTo) { biome = &b; break; }
    }

    if (!biome->floors.empty())
        d.floor = biome->floors[(hg >> 4) % biome->floors.size()];
    d.floorTurn = static_cast<int>((hg >> 12) & 3u);

    // Does something grow here? Walls, water, and cells reserved for a spawned
    // entity stay bare.
    const bool bare = d.solid || (k && (!k->props || !k->spawn.empty()));
    if (!bare) {
        int roll = static_cast<int>(hash32(seed, x, z, SALT_PROP) % 1000u);
        for (const PropRule& rule : biome->props) {
            if (roll < rule.perMille) {
                const uint32_t hp = hash32(seed, x, z, SALT_PLACE);
                const int span = 2 * PROP_JITTER_STEPS + 1;
                const int jx = static_cast<int>(hp % span) - PROP_JITTER_STEPS;
                const int jz = static_cast<int>((hp >> 8) % span) - PROP_JITTER_STEPS;
                d.hasProp = true;
                d.propCell = rule.cell;
                d.propScale = rule.scale;
                d.propX = x + 0.5f + jx * PROP_JITTER_UNIT;
                d.propZ = z + 0.5f + jz * PROP_JITTER_UNIT;
                d.propFlip = ((hp >> 16) & 1u) != 0;
                d.propSolid = rule.solid;
                d.propRadius = rule.radius;
                break;
            }
            roll -= rule.perMille;
        }
    }
    return d;
}

bool Terrain::blocksCircle(float x, float z, float r) const {
    if (!enabled) return false;
    const int x0 = cellOf(x - r - PROP_REACH), x1 = cellOf(x + r + PROP_REACH);
    const int z0 = cellOf(z - r - PROP_REACH), z1 = cellOf(z + r + PROP_REACH);
    for (int cz = z0; cz <= z1; ++cz) {
        for (int cx = x0; cx <= x1; ++cx) {
            const CellDesc d = describe(cx, cz);
            if (d.solid) {
                // Circle vs the cell's square: distance to the closest point.
                float nx = x < cx ? static_cast<float>(cx)
                         : (x > cx + 1 ? static_cast<float>(cx + 1) : x);
                float nz = z < cz ? static_cast<float>(cz)
                         : (z > cz + 1 ? static_cast<float>(cz + 1) : z);
                float dx = x - nx, dz = z - nz;
                if (dx * dx + dz * dz < r * r) return true;
            }
            if (d.hasProp && d.propSolid) {
                float dx = x - d.propX, dz = z - d.propZ;
                float reach = r + d.propRadius;
                if (dx * dx + dz * dz < reach * reach) return true;
            }
        }
    }
    return false;
}

float Terrain::sightClearance(Vec3 from, Vec3 to) const {
    if (!enabled) return 1.0f;
    const Vec3 span = to - from;
    const float len = length(span);
    if (len <= 0.0f) return 1.0f;
    const int steps = static_cast<int>(len / SIGHT_STEP) + 1;
    for (int i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / steps;
        const Vec3 p = from + span * t;
        if (static_cast<float>(wallHeightAt(cellOf(p.x), cellOf(p.z))) > p.y) {
            const float clear = static_cast<float>(i - 1) / steps - SIGHT_MARGIN / len;
            return clear > 0.0f ? clear : 0.0f;
        }
    }
    return 1.0f;
}

void Terrain::buildCell(int x, int z, std::vector<Instance>& out) const {
    const CellDesc d = describe(x, z);
    if (!d.exists) return;

    if (d.height == 0) {
        out.push_back(floorQuad(x, z, d, sheet));
    } else {
        // A wall: one quad per exposed face per storey, then the top. Faces
        // against a neighbour at least as tall are hidden, so they are skipped.
        struct Face { int dx, dz; float px, pz, yaw, shade; };
        const Face faces[4] = {
            { 0,  1, x + 0.5f, z + 1.0f, 0.0f,    SHADE_SOUTH},
            { 0, -1, x + 0.5f, z + 0.0f, 0.0f,    SHADE_NORTH},
            { 1,  0, x + 1.0f, z + 0.5f, HALF_PI, SHADE_EAST },
            {-1,  0, x + 0.0f, z + 0.5f, HALF_PI, SHADE_WEST },
        };
        for (const Face& f : faces) {
            const int neighbour = wallHeightAt(x + f.dx, z + f.dz);
            for (int level = neighbour; level < d.height; ++level) {
                Instance q = makeQuad({f.px, level + 0.5f, f.pz}, {1, 1}, f.yaw, 0.0f);
                q.sheet = static_cast<float>(sheet);
                const float s = f.shade * d.shade;
                q.tint = {s, s, s, 1.0f};
                setAnimation(q, level == 0 ? d.wallBase : d.wall, 1, 0.0f, 0.0f);
                out.push_back(q);
            }
        }
        Instance top = makeQuad({x + 0.5f, static_cast<float>(d.height), z + 0.5f},
                                {1, 1}, 0.0f, -HALF_PI);
        top.sheet = static_cast<float>(sheet);
        const float s = SHADE_TOP * d.shade;
        top.tint = {s, s, s, 1.0f};
        setAnimation(top, d.top, 1, 0.0f, 0.0f);
        out.push_back(top);
    }

    if (d.hasProp) {
        // Anchored at its foot so it stands on the ground from any camera angle.
        Vec2 size = d.propScale;
        Instance p = makeBillboard({d.propX, 0.0f, d.propZ},
                                   {d.propFlip ? -size.x : size.x, size.y});
        p.offset = {0.0f, size.y * 0.5f};
        p.sheet = static_cast<float>(sheet);
        p.tint = {d.shade, d.shade, d.shade, 1.0f};
        setAnimation(p, d.propCell, 1, 0.0f, 0.0f);
        out.push_back(p);
    }
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

bool readTerrain(const JsonValue& t, Terrain& out,
                 std::vector<TerrainSpawn>& spawns, std::string& error) {
    if (!t.isObject()) return fail(error, "terrain", "must be an object");
    Terrain ter;
    ter.enabled = true;
    if (const JsonValue* v = t.find("sheet"))  ter.sheet = static_cast<int>(v->number(0));
    if (const JsonValue* v = t.find("seed"))   ter.seed = static_cast<uint32_t>(v->number(1));
    if (const JsonValue* v = t.find("radius")) ter.radius = static_cast<int>(v->number(ter.radius));
    if (ter.radius < 1 || ter.radius > 64)
        return fail(error, "terrain.radius", "must be between 1 and 64");
    if (const JsonValue* o = t.find("origin")) {
        if (!o->isArray() || o->arr.size() != 2)
            return fail(error, "terrain.origin", "expected [x, z]");
        ter.originX = static_cast<int>(o->arr[0].number());
        ter.originZ = static_cast<int>(o->arr[1].number());
    }

    // Kind 0 is the implicit "wild" kind every unlisted space character maps to.
    ter.kinds.push_back(TileKind{});
    int kindOfChar[256];
    for (int& k : kindOfChar) k = -1;
    kindOfChar[static_cast<unsigned char>(' ')] = 0;

    if (const JsonValue* legend = t.find("legend")) {
        if (!legend->isObject()) return fail(error, "terrain.legend", "must be an object");
        for (const auto& kv : legend->obj) {
            const std::string ctx = "terrain.legend['" + kv.first + "']";
            if (kv.first.size() != 1)
                return fail(error, ctx, "legend keys must be a single character");
            if (ter.kinds.size() >= 255)
                return fail(error, ctx, "too many legend entries");
            const JsonValue& e = kv.second;
            TileKind kind;
            if (const JsonValue* f = e.find("floor")) {
                kind.wild = false;
                kind.floor = static_cast<int>(f->number(0));
            }
            if (const JsonValue* v = e.find("floorFrames")) kind.floorCount = static_cast<int>(v->number(1));
            if (const JsonValue* v = e.find("floorFps"))    kind.floorFps = static_cast<float>(v->number(0));
            if (const JsonValue* v = e.find("wall"))   kind.wall = static_cast<int>(v->number(-1));
            if (const JsonValue* v = e.find("wallBase")) kind.wallBase = static_cast<int>(v->number(-1));
            if (const JsonValue* v = e.find("top"))    kind.top = static_cast<int>(v->number(-1));
            if (const JsonValue* v = e.find("height")) kind.height = static_cast<int>(v->number(0));
            if (const JsonValue* v = e.find("solid"))  kind.solid = v->boolean(false);
            if (const JsonValue* v = e.find("props"))  kind.props = v->boolean(true);
            if (const JsonValue* v = e.find("spawn")) {
                // One archetype name, or a list (e.g. an enemy and its shim).
                if (v->isArray()) {
                    for (const JsonValue& a : v->arr) kind.spawn.push_back(a.string());
                } else {
                    kind.spawn.push_back(v->string());
                }
            }
            if (kind.wall >= 0 && kind.height < 1) kind.height = 1;
            if (kind.height > 8) return fail(error, ctx, "height must be at most 8");
            if (kind.floorCount < 1) return fail(error, ctx, "floorFrames must be at least 1");
            kindOfChar[static_cast<unsigned char>(kv.first[0])] = static_cast<int>(ter.kinds.size());
            ter.kinds.push_back(std::move(kind));
        }
    }

    if (const JsonValue* rows = t.find("rows")) {
        if (!rows->isArray()) return fail(error, "terrain.rows", "must be an array of strings");
        ter.height = static_cast<int>(rows->arr.size());
        for (int z = 0; z < ter.height; ++z) {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "terrain.rows[%d]", z);
            const JsonValue& row = rows->arr[z];
            if (row.type != JsonValue::Type::String) return fail(error, buf, "must be a string");
            const std::string& s = row.strVal;
            if (z == 0) ter.width = static_cast<int>(s.size());
            if (static_cast<int>(s.size()) != ter.width)
                return fail(error, buf, "every row must have the same length as rows[0]");
            for (int x = 0; x < ter.width; ++x) {
                const int kind = kindOfChar[static_cast<unsigned char>(s[x])];
                if (kind < 0)
                    return fail(error, buf, std::string("character '") + s[x]
                                + "' is not in the legend");
                ter.cells.push_back(static_cast<uint8_t>(kind));
                for (const std::string& archetype : ter.kinds[kind].spawn)
                    spawns.push_back({archetype, ter.originX + x, ter.originZ + z});
            }
        }
    }

    if (const JsonValue* w = t.find("wild")) {
        ter.wild = true;
        if (const JsonValue* v = w->find("scale")) ter.biomeScale = static_cast<int>(v->number(ter.biomeScale));
        if (ter.biomeScale < 2 || ter.biomeScale > 64)
            return fail(error, "terrain.wild.scale", "must be between 2 and 64");
        const JsonValue* biomes = w->find("biomes");
        if (!biomes || !biomes->isArray() || biomes->arr.empty())
            return fail(error, "terrain.wild.biomes", "must be a non-empty array");
        for (size_t i = 0; i < biomes->arr.size(); ++i) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "terrain.wild.biomes[%zu]", i);
            const JsonValue& b = biomes->arr[i];
            Biome biome;
            if (const JsonValue* v = b.find("name")) biome.name = v->string();
            if (const JsonValue* v = b.find("upTo")) biome.upTo = static_cast<int>(v->number(256));
            const JsonValue* floors = b.find("floor");
            if (!floors) return fail(error, buf, "missing 'floor'");
            if (floors->isArray()) {
                for (const JsonValue& f : floors->arr)
                    biome.floors.push_back(static_cast<int>(f.number(0)));
            } else {
                biome.floors.push_back(static_cast<int>(floors->number(0)));
            }
            if (biome.floors.empty()) return fail(error, buf, "'floor' must not be empty");
            int total = 0;
            if (const JsonValue* props = b.find("props")) {
                if (!props->isArray()) return fail(error, buf, "'props' must be an array");
                for (const JsonValue& p : props->arr) {
                    PropRule rule;
                    if (const JsonValue* v = p.find("cell"))     rule.cell = static_cast<int>(v->number(0));
                    if (const JsonValue* v = p.find("perMille")) rule.perMille = static_cast<int>(v->number(0));
                    if (const JsonValue* v = p.find("solid"))    rule.solid = v->boolean(false);
                    if (const JsonValue* v = p.find("radius"))   rule.radius = static_cast<float>(v->number(rule.radius));
                    if (const JsonValue* v = p.find("scale")) {
                        if (!v->isArray() || v->arr.size() != 2)
                            return fail(error, buf, "prop 'scale' must be [w, h]");
                        rule.scale = {v->at(0), v->at(1)};
                    }
                    if (rule.perMille < 0) return fail(error, buf, "prop 'perMille' must not be negative");
                    if (rule.radius < 0.0f || rule.radius > 0.5f)
                        return fail(error, buf, "prop 'radius' must be between 0 and 0.5");
                    total += rule.perMille;
                    biome.props.push_back(rule);
                }
            }
            if (total > 1000) return fail(error, buf, "prop chances add up to more than 1000");
            ter.biomes.push_back(std::move(biome));
        }
    }

    out = std::move(ter);
    return true;
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

void TerrainStreamer::reset(Diff& out) {
    for (const auto& kv : loaded_)
        for (EntityId id : kv.second) out.removals.push_back(id);
    loaded_.clear();
    freeIds_.clear();
    nextId_ = TERRAIN_ID_BASE;
    hasFocus_ = false;
}

void TerrainStreamer::update(const Terrain& terrain, Vec3 focus, Diff& out) {
    if (!terrain.enabled) return;
    const int fx = cellOf(focus.x), fz = cellOf(focus.z);
    if (hasFocus_ && fx == focusX_ && fz == focusZ_) return;
    hasFocus_ = true;
    focusX_ = fx;
    focusZ_ = fz;

    // Unload a little later than we load, so pacing back and forth across a
    // cell border doesn't rebuild the same ring of cells every step.
    // Ids released here are only recycled on a LATER update: a Diff applies its
    // upserts before its removals, so reusing one now would delete the new tile.
    const int r = terrain.radius;
    const int keep = r + 2;
    std::vector<EntityId> released;
    for (auto it = loaded_.begin(); it != loaded_.end(); ) {
        const int cx = static_cast<int32_t>(it->first >> 32);
        const int cz = static_cast<int32_t>(it->first & 0xFFFFFFFFu);
        const int dx = cx - fx, dz = cz - fz;
        if (dx * dx + dz * dz > keep * keep) {
            for (EntityId id : it->second) {
                out.removals.push_back(id);
                released.push_back(id);
            }
            it = loaded_.erase(it);
        } else {
            ++it;
        }
    }

    for (int dz = -r; dz <= r; ++dz) {
        for (int dx = -r; dx <= r; ++dx) {
            if (dx * dx + dz * dz > r * r) continue;
            const int cx = fx + dx, cz = fz + dz;
            const uint64_t k = key(cx, cz);
            if (loaded_.count(k)) continue;

            scratch_.clear();
            terrain.buildCell(cx, cz, scratch_);
            std::vector<EntityId> ids;
            ids.reserve(scratch_.size());
            for (const Instance& inst : scratch_) {
                EntityId id;
                if (!freeIds_.empty()) { id = freeIds_.back(); freeIds_.pop_back(); }
                else                   { id = nextId_++; }
                out.upserts.emplace_back(id, inst);
                ids.push_back(id);
            }
            loaded_.emplace(k, std::move(ids));
        }
    }
    freeIds_.insert(freeIds_.end(), released.begin(), released.end());
}

} // namespace cv
