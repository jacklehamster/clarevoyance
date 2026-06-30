#include "file_module.h"

#include <sys/stat.h>
#include <fstream>
#include <SDL.h>

#include "third_party/json.hpp"

using json = nlohmann::json;

namespace cv {

static Vec3 toVec3(const json& j, Vec3 def = {0,0,0}) {
    if (!j.is_array() || j.size() < 3) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}
static Vec2 toVec2(const json& j, Vec2 def = {1,1}) {
    if (!j.is_array() || j.size() < 2) return def;
    return {j[0].get<float>(), j[1].get<float>()};
}

static Instance parseInstance(const json& j, const json& entity) {
    Vec2  scale     = entity.contains("scale")     ? toVec2(entity["scale"])  : Vec2{1,1};
    float billboard = entity.value("billboard", 1.0f);

    Instance inst = makeBillboard(toVec3(j.value("pos", json::array())), scale);
    inst.billboard = billboard;
    inst.vel       = toVec3(j.value("vel",   json::array()));
    inst.accel     = toVec3(j.value("accel", json::array()));

    if (entity.contains("anim")) {
        const auto& a = entity["anim"];
        setAnimation(inst,
            a.value("first", 0),
            a.value("count", 1),
            a.value("fps",   0.0f),
            0.0f);
    }
    if (j.contains("tint")) {
        const auto& t = j["tint"];
        if (t.is_array() && t.size() >= 4)
            inst.tint = {t[0], t[1], t[2], t[3]};
    }
    return inst;
}

// ---------------------------------------------------------------------------

FileModule::FileModule(const char* path) : path_(path) {}

void FileModule::tick(float simTime, ScriptRunner& runner, Diff& diff) {
    struct stat st{};
    if (stat(path_.c_str(), &st) != 0) return;
    if (st.st_mtime == lastMod_) return;

    // File is new or changed — clear existing scripts and reload.
    // (ScriptRunner::clear() would be cleaner; for now we rely on replace.)
    reload(simTime, runner, diff);
    lastMod_  = st.st_mtime;
    loadTime_ = simTime;
}

void FileModule::reload(float simTime, ScriptRunner& runner, Diff& diff) {
    std::ifstream f(path_);
    if (!f.is_open()) {
        SDL_Log("FileModule: cannot open %s", path_.c_str());
        return;
    }

    json root;
    try { root = json::parse(f); }
    catch (const json::exception& e) {
        SDL_Log("FileModule: JSON parse error in %s: %s", path_.c_str(), e.what());
        return;
    }

    SDL_Log("FileModule: loading %s", path_.c_str());

    if (!root.contains("entities")) return;

    for (const auto& e : root["entities"]) {
        EntityId id = e.value("id", 0);
        if (id == 0) continue;

        EntityScript script;
        script.id = id;

        if (e.contains("phases")) {
            script.loop = e.value("loop", false);
            for (const auto& p : e["phases"]) {
                float dur = p.value("duration", 1.0f);
                Phase phase;
                phase.inst = parseInstance(p, e);
                phase.exit = exitAfter(dur);
                script.phases.push_back(phase);
            }
            runner.replace(std::move(script), simTime, diff);

        } else if (e.contains("repeat")) {
            const auto& r   = e["repeat"];
            float damping   = r.value("damping", 0.6f);
            float minVel    = r.value("minVel",  0.4f);

            script.hasRepeat     = true;
            script.repeat.seed   = parseInstance(r["seed"], e);
            script.repeat.exit   = exitOnGround(0.0f);
            script.repeat.transition = [damping](const Instance& prev, float t) {
                float dt       = t - prev.motionStart;
                float vyImpact = prev.vel.y + prev.accel.y * dt;
                Instance next  = prev;
                next.pos.x    += prev.vel.x * dt + 0.5f * prev.accel.x * dt * dt;
                next.pos.y     = 0.0f;
                next.pos.z    += prev.vel.z * dt + 0.5f * prev.accel.z * dt * dt;
                next.vel.x    += prev.accel.x * dt;
                next.vel.y     = -vyImpact * damping;
                next.vel.z    += prev.accel.z * dt;
                next.motionStart = t;
                return next;
            };
            script.repeat.stop = [minVel](const Instance& inst) {
                return std::abs(inst.vel.y) < minVel;
            };
            runner.replace(std::move(script), simTime, diff);
        }
    }
}

} // namespace cv
