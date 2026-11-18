// scene.cpp — parse a JSON scene file into a Scene (WorldState + events).
//
// The loader is STRICT (MAP_EDITOR spec): unknown trigger/action types, unknown
// condition ops, duplicate entity ids, unresolved entity/archetype/clip
// references, and malformed vectors are hard errors with contextual messages
// (e.g. "events[3].actions[0]: unknown action type 'set_flg'"). Scenes carry a
// top-level "version" (currently 1): missing warns on stderr, newer errors.
#include "scene.h"
#include "json.h"
#include "sim.h"
#include "text.h"

#include <cmath>
#include <cstdio>

namespace cv {

namespace {

// The newest scene format version this loader understands.
constexpr int SCENE_VERSION = 1;

// "events[3]" — index a context path segment.
std::string ctxIndex(const std::string& name, size_t i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "[%zu]", i);
    return name + buf;
}

bool failCtx(std::string& error, const std::string& ctx, const std::string& msg) {
    error = ctx + ": " + msg;
    return false;
}

// --- Strict vector readers ----------------------------------------------------
// Absent field → keep the caller's default. Present but not an array of exactly
// N numbers → error.

bool numbersOnly(const JsonValue& v) {
    for (const JsonValue& x : v.arr)
        if (x.type != JsonValue::Type::Number) return false;
    return true;
}

bool readVec3(const JsonValue* v, Vec3& out, const std::string& ctx, std::string& error) {
    if (!v) return true;
    if (!v->isArray() || v->arr.size() != 3 || !numbersOnly(*v))
        return failCtx(error, ctx, "expected an array of 3 numbers");
    out = { v->at(0), v->at(1), v->at(2) };
    return true;
}

bool readVec2(const JsonValue* v, Vec2& out, const std::string& ctx, std::string& error) {
    if (!v) return true;
    if (!v->isArray() || v->arr.size() != 2 || !numbersOnly(*v))
        return failCtx(error, ctx, "expected an array of 2 numbers");
    out = { v->at(0), v->at(1) };
    return true;
}

bool readVec4(const JsonValue* v, Vec4& out, const std::string& ctx, std::string& error) {
    if (!v) return true;
    if (!v->isArray() || v->arr.size() != 4 || !numbersOnly(*v))
        return failCtx(error, ctx, "expected an array of 4 numbers");
    out = { v->at(0), v->at(1), v->at(2), v->at(3) };
    return true;
}

bool readCamera(const JsonValue& c, Camera& cam, const std::string& ctx, std::string& error) {
    if (const JsonValue* p = c.find("projection"))
        cam.projection = (p->string() == "orthographic")
            ? Projection::Orthographic : Projection::Perspective;
    if (!readVec3(c.find("position"), cam.position, ctx + ".position", error)) return false;
    if (!readVec3(c.find("target"),   cam.target,   ctx + ".target",   error)) return false;
    if (!readVec3(c.find("up"),       cam.up,       ctx + ".up",       error)) return false;
    if (const JsonValue* fov = c.find("fovY")) cam.fovYRadians = static_cast<float>(fov->number(cam.fovYRadians));
    if (const JsonValue* oh = c.find("orthoHalfHeight"))
        cam.orthoHalfHeight = static_cast<float>(oh->number(cam.orthoHalfHeight));
    return true;
}

// Parse an archetype's "clips" map: clip name → {first, count, fps}.
Archetype readArchetype(const JsonValue& a) {
    Archetype arch;
    if (const JsonValue* clips = a.find("clips")) {
        if (clips->isObject()) {
            for (const auto& kv : clips->obj) {
                Clip clip;
                if (const JsonValue* f = kv.second.find("first"))
                    clip.first = static_cast<int>(f->number());
                if (const JsonValue* c = kv.second.find("count"))
                    clip.count = static_cast<int>(c->number(1));
                if (const JsonValue* fps = kv.second.find("fps"))
                    clip.fps = static_cast<float>(fps->number());
                arch.clips[kv.first] = clip;
            }
        }
    }
    return arch;
}

// Merge an entity JSON object over its archetype's fields: archetype fields
// apply as defaults, entity fields override key-by-key. "clips" is archetype
// metadata (not an entity field) and is skipped.
JsonValue mergeWithArchetype(const JsonValue& archetype, const JsonValue& entity) {
    JsonValue merged;
    merged.type = JsonValue::Type::Object;
    for (const auto& kv : archetype.obj)
        if (kv.first != "clips")
            merged.obj.emplace_back(kv.first, kv.second);
    for (const auto& kv : entity.obj) {
        bool replaced = false;
        for (auto& mkv : merged.obj)
            if (mkv.first == kv.first) { mkv.second = kv.second; replaced = true; break; }
        if (!replaced) merged.obj.emplace_back(kv.first, kv.second);
    }
    return merged;
}

// Build an entity's renderable Instance and fill its game-side attributes
// (controlled/speed) from the same JSON object. `arch` (may be null) supplies
// the named clips a string-valued "anim" field resolves against.
bool readEntity(const JsonValue& e, EntityAttrs& attrs, const Archetype* arch,
                const std::string& archName, Instance& out,
                const std::string& ctx, std::string& error) {
    Vec3 pos   = {0, 0, 0};
    Vec2 scale = {1, 1};
    if (!readVec3(e.find("pos"),   pos,   ctx + ".pos",   error)) return false;
    if (!readVec2(e.find("scale"), scale, ctx + ".scale", error)) return false;
    bool bb = true;
    if (const JsonValue* b = e.find("billboard")) bb = b->boolean(true);

    Instance inst = bb ? makeBillboard(pos, scale) : makeSprite(pos, scale);
    // Non-billboard orientation (radians): "rotation" = yaw about world-up,
    // "pitch" tilts the quad (0 = upright wall, -pi/2 = flat floor tile).
    if (!bb && e.find("rotation"))
        inst.rotation = static_cast<float>(e.find("rotation")->number());
    if (!bb && e.find("pitch"))
        inst.pitch = static_cast<float>(e.find("pitch")->number());

    if (!readVec3(e.find("vel"),   inst.vel,   ctx + ".vel",   error)) return false;
    if (!readVec3(e.find("accel"), inst.accel, ctx + ".accel", error)) return false;

    // Optional motion time-origin override (default 0, i.e. sim start). Mainly
    // for authoring a "shim" ghost: give it the same pos/vel as another entity
    // but motionStart = -N, and its position formula renders N seconds ahead
    // of that entity's — a real lookahead preview built from nothing but the
    // existing motion model (see quest.json's enemy / enemy_shim).
    if (const JsonValue* ms = e.find("motionStart"))
        inst.motionStart = static_cast<float>(ms->number(inst.motionStart));

    // Optional sheet index into the scene's "sheets" list (default 0).
    if (const JsonValue* sh = e.find("sheet"))
        inst.sheet = static_cast<float>(static_cast<int>(sh->number(0)));

    // Optional RGBA tint multiplier; alpha < 1 renders in the translucent pass.
    if (!readVec4(e.find("tint"), inst.tint, ctx + ".tint", error)) return false;

    // Animation: either an inline object {first, count, fps, start} or a string
    // naming a clip from the entity's archetype ("anim": "walk").
    if (const JsonValue* a = e.find("anim")) {
        if (a->type == JsonValue::Type::String) {
            if (!arch)
                return failCtx(error, ctx + ".anim",
                               "clip name '" + a->strVal + "' requires an archetype");
            auto it = arch->clips.find(a->strVal);
            if (it == arch->clips.end())
                return failCtx(error, ctx + ".anim",
                               "unknown clip '" + a->strVal + "' (archetype '" + archName + "')");
            setAnimation(inst, it->second.first, it->second.count, it->second.fps, 0.0f);
        } else {
            int   first = static_cast<int>(a->find("first") ? a->find("first")->number() : 0);
            int   count = static_cast<int>(a->find("count") ? a->find("count")->number() : 1);
            float fps   = a->find("fps") ? static_cast<float>(a->find("fps")->number()) : 0.0f;
            float start = a->find("start") ? static_cast<float>(a->find("start")->number()) : 0.0f;
            setAnimation(inst, first, count, fps, start);
        }
    }

    // Game-side attributes (optional): movement input applies to any entity with
    // controlled=true; speed is its movement rate in world units / second.
    if (const JsonValue* c = e.find("controlled")) attrs.controlled = c->boolean(false);
    if (const JsonValue* s = e.find("speed"))      attrs.speed = static_cast<float>(s->number(attrs.speed));

    // Pivot offset within the quad's own plane (world units). [0, h/2] stands a
    // billboard on its feet: `pos` is then the point where it touches the ground.
    if (!readVec2(e.find("offset"), inst.offset, ctx + ".offset", error)) return false;
    if (const JsonValue* x = e.find("xray")) inst.xray = x->boolean(false) ? 1.0f : 0.0f;

    // Collision: a circle of `radius` around pos; `solid` entities block the
    // controlled ones.
    if (const JsonValue* r = e.find("radius")) attrs.radius = static_cast<float>(r->number(attrs.radius));
    if (const JsonValue* s = e.find("solid"))  attrs.solid = s->boolean(false);
    if (attrs.radius < 0.0f || attrs.radius > 0.5f)
        return failCtx(error, ctx + ".radius", "must be between 0 and 0.5");

    if (const JsonValue* aa = e.find("autoAnim")) {
        attrs.autoAnim = aa->boolean(false);
        if (attrs.autoAnim && (!arch || !arch->clips.count("walk") || !arch->clips.count("idle")))
            return failCtx(error, ctx + ".autoAnim",
                           "requires an archetype with 'walk' and 'idle' clips");
    }

    // Patrol: walk to pos + delta and back, forever. `phase` (seconds) starts
    // the patrol that far along — give a translucent twin a phase of 1 and it
    // shows where the original will be one second from now.
    if (const JsonValue* p = e.find("patrol")) {
        Vec3 delta = {0, 0, 0};
        if (!readVec3(p->find("delta"), delta, ctx + ".patrol.delta", error)) return false;
        float speed = p->find("speed") ? static_cast<float>(p->find("speed")->number(1.0)) : 1.0f;
        float phase = p->find("phase") ? static_cast<float>(p->find("phase")->number(0.0)) : 0.0f;
        float dist = length(delta);
        if (dist <= 0.0f)  return failCtx(error, ctx + ".patrol.delta", "must not be zero");
        if (speed <= 0.0f) return failCtx(error, ctx + ".patrol.speed", "must be positive");
        if (phase < 0.0f)  return failCtx(error, ctx + ".patrol.phase", "must not be negative");
        attrs.patrol.enabled = true;
        attrs.patrol.a = inst.pos;
        attrs.patrol.b = inst.pos + delta;
        long legTicks = std::lround(dist / speed / SIM_DT);
        attrs.patrol.legTicks = static_cast<uint32_t>(legTicks < 1 ? 1 : legTicks);
        attrs.patrol.phaseTicks = static_cast<uint32_t>(std::lround(phase / SIM_DT));
        Vec3 vel;
        patrolPose(attrs.patrol, 0, inst.pos, vel, inst.motionStart);
        inst.vel = vel;
        attrs.faceDir = vel;
    }

    // Labels hang from the sprite's feet: `pos` itself when a pivot offset
    // stands it on the ground, otherwise the bottom edge of the quad.
    attrs.labelDy = e.find("offset") ? 0.0f : -std::fabs(inst.scale.y) * 0.5f;
    out = inst;
    return true;
}

double numOrBool(const JsonValue* v, double fallback);
bool readCondition(const JsonValue& c, Condition& out,
                   const std::string& ctx, std::string& error);

// {"name": amount, ...} — flags to set, or items to give/take. Booleans are 0/1.
bool readAmounts(const JsonValue* v, Amounts& out, const std::string& ctx, std::string& error) {
    if (!v) return true;
    if (!v->isObject()) return failCtx(error, ctx, "must be an object of name: number");
    for (const auto& kv : v->obj) {
        if (kv.second.type != JsonValue::Type::Number && kv.second.type != JsonValue::Type::Bool)
            return failCtx(error, ctx + "." + kv.first, "must be a number or boolean");
        out.emplace_back(kv.first, numOrBool(&kv.second, 1.0));
    }
    return true;
}

// One line of a conversation (see DialogueLine in scene.h).
bool readDialogueLine(const JsonValue& l, const Scene& scene, DialogueLine& out,
                      const std::string& ctx, std::string& error) {
    if (!l.isObject()) return failCtx(error, ctx, "must be an object");
    DialogueLine line;
    if (const JsonValue* c = l.find("if"))
        if (!readCondition(*c, line.condition, ctx + ".if", error)) return false;
    if (const JsonValue* sp = l.find("speaker")) {
        line.speaker = sp->string();
        if (!scene.speakers.count(line.speaker))
            return failCtx(error, ctx + ".speaker",
                           "'" + line.speaker + "' is not an archetype");
    }
    if (const JsonValue* t = l.find("text")) line.text = t->string();
    if (line.text.empty()) return failCtx(error, ctx, "missing 'text'");
    if (!readAmounts(l.find("set"),  line.set,  ctx + ".set",  error)) return false;
    if (!readAmounts(l.find("give"), line.give, ctx + ".give", error)) return false;
    if (!readAmounts(l.find("take"), line.take, ctx + ".take", error)) return false;
    if (const JsonValue* e = l.find("end")) line.end = e->boolean(false);
    if (const JsonValue* choices = l.find("choices")) {
        if (!choices->isArray() || choices->arr.empty())
            return failCtx(error, ctx + ".choices", "must be a non-empty array");
        for (size_t i = 0; i < choices->arr.size(); ++i) {
            const JsonValue& c = choices->arr[i];
            const std::string cctx = ctxIndex(ctx + ".choices", i);
            DialogueChoice choice;
            if (const JsonValue* t = c.find("text")) choice.text = t->string();
            if (choice.text.empty()) return failCtx(error, cctx, "missing 'text'");
            if (!readAmounts(c.find("set"), choice.set, cctx + ".set", error)) return false;
            line.choices.push_back(std::move(choice));
        }
    }
    out = std::move(line);
    return true;
}

bool readRigView(const JsonValue* v, RigView& view, const std::string& ctx, std::string& error) {
    if (!v) return true;
    if (!v->isObject()) return failCtx(error, ctx, "must be an object");
    auto num = [&](const char* key, float& dst) {
        if (const JsonValue* x = v->find(key)) dst = static_cast<float>(x->number(dst));
    };
    num("back", view.back);
    num("height", view.height);
    num("lookAhead", view.lookAhead);
    num("targetHeight", view.targetHeight);
    num("fovY", view.fovY);
    if (view.back < 0.0f || view.height <= 0.0f || view.fovY <= 0.0f)
        return failCtx(error, ctx, "back must be >= 0; height and fovY must be positive");
    return true;
}

// Fail unless `name` is a defined entity. `field` names the JSON key at fault.
bool checkEntityRef(const Scene& scene, const std::string& name, const char* field,
                    const std::string& ctx, std::string& error) {
    if (name.empty())
        return failCtx(error, ctx, std::string("missing '") + field + "'");
    if (scene.nameToId.find(name) == scene.nameToId.end())
        return failCtx(error, ctx, std::string("'") + field
                       + "' references unknown entity '" + name + "'");
    return true;
}

bool readTrigger(const JsonValue& t, const Scene& scene, Trigger& out,
                 const std::string& ctx, std::string& error) {
    Trigger tr;
    std::string type = t.find("type") ? t.find("type")->string() : "start";
    if (type == "proximity") {
        tr.type = Trigger::Type::Proximity;
        tr.entity = t.find("entity") ? t.find("entity")->string() : "";
        tr.target = t.find("target") ? t.find("target")->string() : "";
        tr.radius = t.find("radius") ? static_cast<float>(t.find("radius")->number()) : 1.0f;
        if (!checkEntityRef(scene, tr.entity, "entity", ctx, error)) return false;
        if (!checkEntityRef(scene, tr.target, "target", ctx, error)) return false;
    } else if (type == "input") {
        tr.type = Trigger::Type::Input;
        tr.action = t.find("action") ? t.find("action")->string() : "";
        std::string edge = t.find("edge") ? t.find("edge")->string() : "pressed";
        if (edge == "pressed")       tr.edge = Trigger::Edge::Pressed;
        else if (edge == "released") tr.edge = Trigger::Edge::Released;
        else if (edge == "held")     tr.edge = Trigger::Edge::Held;
        else return failCtx(error, ctx, "unknown input edge '" + edge + "'");
    } else if (type == "start") {
        tr.type = Trigger::Type::Start;
    } else if (type == "always") {
        tr.type = Trigger::Type::Always;
    } else {
        return failCtx(error, ctx, "unknown trigger type '" + type + "'");
    }
    out = tr;
    return true;
}

// Flag values are numeric; JSON booleans map to 0/1 so data files may keep
// writing `"value": true` for boolean-style flags.
double numOrBool(const JsonValue* v, double fallback) {
    if (!v) return fallback;
    if (v->type == JsonValue::Type::Bool) return v->boolVal ? 1.0 : 0.0;
    return v->number(fallback);
}

// Recursive condition parse: {"all":[...]} / {"any":[...]} composition, or a
// single flag comparison {"flag": name, "value": x, "op": "eq"|"ne"|...}.
bool readCondition(const JsonValue& c, Condition& out,
                   const std::string& ctx, std::string& error) {
    Condition cond;
    const JsonValue* all = c.find("all");
    const JsonValue* any = c.find("any");
    if (all || any) {
        const JsonValue* list = all ? all : any;
        const char* key = all ? "all" : "any";
        cond.kind = all ? Condition::Kind::All : Condition::Kind::Any;
        if (!list->isArray())
            return failCtx(error, ctx, std::string("'") + key + "' must be an array");
        for (size_t i = 0; i < list->arr.size(); ++i) {
            Condition child;
            if (!readCondition(list->arr[i], child,
                               ctxIndex(ctx + "." + key, i), error))
                return false;
            cond.children.push_back(std::move(child));
        }
        out = std::move(cond);
        return true;
    }
    // Inventory shorthands: {"has": "key"} = at least one; {"lacks": "key"} = none.
    if (const JsonValue* has = c.find("has")) {
        cond.kind = Condition::Kind::Item;
        cond.flag = has->string();
        cond.op = Condition::Op::Ge;
        cond.value = 1.0;
        out = std::move(cond);
        return true;
    }
    if (const JsonValue* lacks = c.find("lacks")) {
        cond.kind = Condition::Kind::Item;
        cond.flag = lacks->string();
        cond.op = Condition::Op::Lt;
        cond.value = 1.0;
        out = std::move(cond);
        return true;
    }
    // A comparison on a flag, or on an item count: {"item": "herb", "op": "ge", "value": 3}.
    const JsonValue* subject = c.find("flag");
    cond.kind = Condition::Kind::Flag;
    if (!subject) {
        subject = c.find("item");
        cond.kind = Condition::Kind::Item;
    }
    if (!subject)
        return failCtx(error, ctx,
                       "condition must have 'flag', 'item', 'has', 'lacks', 'all', or 'any'");
    cond.flag  = subject->string();
    cond.value = numOrBool(c.find("value"), 1.0);
    std::string op = c.find("op") ? c.find("op")->string() : "eq";
    if      (op == "eq") cond.op = Condition::Op::Eq;
    else if (op == "ne") cond.op = Condition::Op::Ne;
    else if (op == "lt") cond.op = Condition::Op::Lt;
    else if (op == "le") cond.op = Condition::Op::Le;
    else if (op == "gt") cond.op = Condition::Op::Gt;
    else if (op == "ge") cond.op = Condition::Op::Ge;
    else return failCtx(error, ctx, "unknown condition op '" + op + "'");
    out = std::move(cond);
    return true;
}

bool readAction(const JsonValue& a, const Scene& scene, Action& out,
                const std::string& ctx, std::string& error) {
    Action ac;
    std::string type = a.find("type") ? a.find("type")->string() : "dialogue";
    if (type == "set_flag") {
        ac.type  = Action::Type::SetFlag;
        ac.flag  = a.find("flag") ? a.find("flag")->string() : "";
        ac.value = numOrBool(a.find("value"), 1.0);
    } else if (type == "add_flag") {
        ac.type  = Action::Type::AddFlag;
        ac.flag  = a.find("flag") ? a.find("flag")->string() : "";
        ac.value = numOrBool(a.find("value"), 1.0);
    } else if (type == "set_anim") {
        ac.type   = Action::Type::SetAnim;
        ac.entity = a.find("entity") ? a.find("entity")->string() : "";
        if (!checkEntityRef(scene, ac.entity, "entity", ctx, error)) return false;
        // Either a named clip (resolved through the target entity's archetype
        // at runtime) or raw first/count/fps. Clip references are validated now.
        ac.clip   = a.find("clip") ? a.find("clip")->string() : "";
        if (!ac.clip.empty()) {
            if (!scene.clipOf(scene.idOf(ac.entity), ac.clip))
                return failCtx(error, ctx, "clip '" + ac.clip
                               + "' does not resolve for entity '" + ac.entity + "'");
        }
        ac.first  = static_cast<int>(a.find("first") ? a.find("first")->number() : 0);
        ac.count  = static_cast<int>(a.find("count") ? a.find("count")->number() : 1);
        ac.fps    = a.find("fps") ? static_cast<float>(a.find("fps")->number()) : 0.0f;
    } else if (type == "set_motion") {
        ac.type   = Action::Type::SetMotion;
        ac.entity = a.find("entity") ? a.find("entity")->string() : "";
        if (!checkEntityRef(scene, ac.entity, "entity", ctx, error)) return false;
        if (!readVec3(a.find("vel"),   ac.vel,   ctx + ".vel",   error)) return false;
        if (!readVec3(a.find("accel"), ac.accel, ctx + ".accel", error)) return false;
    } else if (type == "remove") {
        ac.type   = Action::Type::Remove;
        ac.entity = a.find("entity") ? a.find("entity")->string() : "";
        if (!checkEntityRef(scene, ac.entity, "entity", ctx, error)) return false;
    } else if (type == "toggle_controlled") {
        ac.type   = Action::Type::ToggleControlled;
        ac.entity = a.find("entity") ? a.find("entity")->string() : "";
        if (!checkEntityRef(scene, ac.entity, "entity", ctx, error)) return false;
    } else if (type == "set_controlled") {
        ac.type   = Action::Type::SetControlled;
        ac.entity = a.find("entity") ? a.find("entity")->string() : "";
        if (!checkEntityRef(scene, ac.entity, "entity", ctx, error)) return false;
        ac.value  = numOrBool(a.find("value"), 1.0);
    } else if (type == "dialogue") {
        ac.type = Action::Type::Dialogue;
        ac.id   = a.find("id") ? a.find("id")->string() : "";
    } else if (type == "talk") {
        // Open a conversation; "entity" (optional) is who it is with.
        ac.type = Action::Type::Talk;
        ac.id   = a.find("id") ? a.find("id")->string() : "";
        if (!scene.dialogues.count(ac.id))
            return failCtx(error, ctx, "unknown dialogue '" + ac.id + "'");
        if (const JsonValue* e = a.find("entity")) {
            ac.entity = e->string();
            if (!checkEntityRef(scene, ac.entity, "entity", ctx, error)) return false;
        }
    } else if (type == "toast") {
        ac.type = Action::Type::Toast;
        ac.id   = a.find("text") ? a.find("text")->string() : "";
        if (ac.id.empty()) return failCtx(error, ctx, "missing 'text'");
    } else if (type == "give") {
        ac.type  = Action::Type::Give;
        ac.flag  = a.find("item") ? a.find("item")->string() : "";
        ac.value = a.find("count") ? a.find("count")->number(1.0) : 1.0;
        if (ac.flag.empty()) return failCtx(error, ctx, "missing 'item'");
    } else {
        return failCtx(error, ctx, "unknown action type '" + type + "'");
    }
    out = ac;
    return true;
}

} // namespace

bool loadScene(const char* path, Scene& out, std::string& error) {
    std::string text;
    if (!readFile(path, text)) { error = std::string("cannot read scene file: ") + path; return false; }

    JsonValue root;
    if (!JsonParser::parse(text, root, error)) return false;
    if (!root.isObject()) { error = "scene root must be a JSON object"; return false; }

    // Version — missing warns (assumed 1); newer than this loader errors.
    if (const JsonValue* v = root.find("version")) {
        int version = static_cast<int>(v->number(0));
        if (version > SCENE_VERSION) {
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "unsupported scene version %d (this loader supports up to %d)",
                          version, SCENE_VERSION);
            error = buf;
            return false;
        }
    } else {
        std::fprintf(stderr, "warning: %s: missing top-level \"version\" (assuming %d)\n",
                     path, SCENE_VERSION);
    }

    // Sheets — preferred: "sheets": [{path, cols, rows}, ...]. The singular
    // "sheet" object is still accepted as sheets[0] for backward compatibility.
    auto readSheet = [](const JsonValue& s) {
        SheetInfo info;
        if (const JsonValue* p = s.find("path")) info.path = p->string(info.path);
        if (const JsonValue* c = s.find("cols")) info.cols = static_cast<int>(c->number(info.cols));
        if (const JsonValue* r = s.find("rows")) info.rows = static_cast<int>(r->number(info.rows));
        return info;
    };
    if (const JsonValue* list = root.find("sheets")) {
        if (list->isArray())
            for (const JsonValue& s : list->arr)
                out.sheets.push_back(readSheet(s));
    } else if (const JsonValue* s = root.find("sheet")) {
        out.sheets.push_back(readSheet(*s));
    }
    if (out.sheets.empty())
        out.sheets.push_back(SheetInfo{});  // sane default (see SheetInfo)

    // Cameras
    if (const JsonValue* cams = root.find("cameras")) {
        if (cams->isArray()) {
            for (size_t i = 0; i < cams->arr.size(); ++i) {
                Camera cam;
                if (!readCamera(cams->arr[i], cam, ctxIndex("cameras", i), error))
                    return false;
                out.initialState.cameras.push_back(cam);
            }
        }
    }
    if (out.initialState.cameras.empty())
        out.initialState.cameras.push_back(Camera{});  // sane default
    if (const JsonValue* ac = root.find("activeCamera"))
        out.initialState.activeCamera = static_cast<int>(ac->number(0));

    // Archetypes — named entity templates with named animation clips. Kept as
    // raw JSON here (for default merging) and parsed onto the Scene (for
    // runtime clip resolution).
    std::unordered_map<std::string, const JsonValue*> archetypeJson;
    if (const JsonValue* archs = root.find("archetypes")) {
        if (archs->isObject()) {
            for (const auto& kv : archs->obj) {
                out.archetypes[kv.first] = readArchetype(kv.second);
                archetypeJson[kv.first]  = &kv.second;
            }
        }
    }

    // Speakers: every archetype can talk. Its face is its own sprite — the
    // "chat" clip if it has one, else "idle", else its inline anim — and its
    // name is "name", else its label, else the archetype's id.
    for (const auto& kv : archetypeJson) {
        const JsonValue& a = *kv.second;
        Speaker sp;
        sp.name = a.find("name") ? a.find("name")->string()
                : a.find("label") ? a.find("label")->string() : kv.first;
        if (const JsonValue* sh = a.find("sheet")) sp.sheet = static_cast<int>(sh->number(0));
        if (!readVec4(a.find("tint"), sp.tint, "archetypes." + kv.first + ".tint", error))
            return false;
        const Archetype& arch = out.archetypes[kv.first];
        auto clip = arch.clips.find("chat");
        if (clip == arch.clips.end()) clip = arch.clips.find("idle");
        if (clip != arch.clips.end()) {
            sp.first = clip->second.first;
            sp.count = clip->second.count;
            sp.fps   = clip->second.fps;
        } else if (const JsonValue* an = a.find("anim")) {
            if (an->isObject() && an->find("first"))
                sp.first = static_cast<int>(an->find("first")->number());
        }
        out.speakers[kv.first] = std::move(sp);
    }

    // Screen overlay: which sheets hold the window pieces and the font.
    if (const JsonValue* ui = root.find("ui")) {
        out.ui.enabled = true;
        if (const JsonValue* v = ui->find("sheet")) out.ui.sheet = static_cast<int>(v->number(0));
        if (const JsonValue* v = ui->find("font"))  out.ui.fontSheet = static_cast<int>(v->number(0));
        const int n = static_cast<int>(out.sheets.size());
        if (out.ui.sheet < 0 || out.ui.sheet >= n || out.ui.fontSheet < 0 || out.ui.fontSheet >= n)
            return failCtx(error, "ui", "'sheet' and 'font' must be indices into 'sheets'");
    }

    // Items: display names for the inventory ({"herb": "Moonherb"} or {"herb": {"name": ...}}).
    if (const JsonValue* items = root.find("items")) {
        if (!items->isObject()) return failCtx(error, "items", "must be an object");
        for (const auto& kv : items->obj) {
            const JsonValue* name = kv.second.isObject() ? kv.second.find("name") : &kv.second;
            out.itemNames[kv.first] = name ? name->string(kv.first) : kv.first;
        }
    }

    // Label style — how entity "label" strings are drawn (see LabelStyle).
    if (const JsonValue* ls = root.find("labelStyle")) {
        if (const JsonValue* v = ls->find("sheet"))   out.labelStyle.sheet = static_cast<int>(v->number(0));
        if (const JsonValue* v = ls->find("height"))  out.labelStyle.height = static_cast<float>(v->number(out.labelStyle.height));
        if (const JsonValue* v = ls->find("advance")) out.labelStyle.advance = static_cast<float>(v->number(out.labelStyle.advance));
        if (const JsonValue* v = ls->find("drop"))    out.labelStyle.drop = static_cast<float>(v->number(out.labelStyle.drop));
    }

    // Terrain — the authored tile map and the procedural wilds around it. Its
    // tiles are NOT entities (the TerrainStreamer instances them on demand);
    // only legend "spawn" entries become entities, below.
    std::vector<TerrainSpawn> spawns;
    if (const JsonValue* t = root.find("terrain")) {
        if (!readTerrain(*t, out.terrain, spawns, error)) return false;
        if (out.terrain.sheet < 0 || out.terrain.sheet >= static_cast<int>(out.sheets.size()))
            return failCtx(error, "terrain.sheet", "is not an index into 'sheets'");
    }

    // Entities — assign numeric ids starting at 1, record name → id. An entity
    // with an "archetype" field takes the archetype's fields as defaults and
    // overrides them key-by-key. Duplicate ids and unknown archetypes are errors.
    EntityId nextId = 1;
    auto addEntity = [&](const JsonValue& e, const std::string& ctx) -> bool {
        std::string name = e.find("id") ? e.find("id")->string() : "";
        if (!name.empty() && out.nameToId.count(name))
            return failCtx(error, ctx, "duplicate entity id '" + name + "'");

        // Text entities ("text": "...") expand to one glyph Instance per
        // character (src/game/text.h) instead of the usual single Instance.
        // The name binds to the FIRST glyph's id; textRangeEnd records the
        // [first,last] id range so `remove` can despawn the whole string.
        if (const JsonValue* textVal = e.find("text")) {
            std::string content = textVal->string();
            Vec3 pos = {0, 0, 0};
            Vec2 charSize = {0.5f, 0.5f};
            Vec4 tint = {1, 1, 1, 1};
            int sheet = 0;
            if (!readVec3(e.find("pos"), pos, ctx + ".pos", error)) return false;
            if (!readVec2(e.find("charSize"), charSize, ctx + ".charSize", error)) return false;
            if (!readVec4(e.find("tint"), tint, ctx + ".tint", error)) return false;
            if (const JsonValue* sh = e.find("sheet")) sheet = static_cast<int>(sh->number(0));

            std::vector<Instance> glyphs =
                makeText(content, pos, charSize.x, charSize.y, sheet, tint);

            EntityId firstId = nextId++;   // reserved even if content is empty
            if (!name.empty()) out.nameToId[name] = firstId;
            if (!glyphs.empty()) {
                out.initialState.instances[firstId] = glyphs[0];
                for (size_t k = 1; k < glyphs.size(); ++k)
                    out.initialState.instances[nextId++] = glyphs[k];
                out.textRangeEnd[firstId] = nextId - 1;
            }
            return true;
        }

        EntityId id = nextId++;
        if (!name.empty()) out.nameToId[name] = id;

        const JsonValue* merged = &e;
        JsonValue mergedStorage;
        const Archetype* arch = nullptr;
        std::string archName;
        if (const JsonValue* an = e.find("archetype")) {
            archName = an->string();
            auto aj = archetypeJson.find(archName);
            if (aj == archetypeJson.end())
                return failCtx(error, ctx, "unknown archetype '" + archName + "'");
            mergedStorage = mergeWithArchetype(*aj->second, e);
            merged = &mergedStorage;
            arch = &out.archetypes[archName];
            out.archetypeOf[id] = archName;
        }

        EntityAttrs attrs;
        Instance inst;
        if (!readEntity(*merged, attrs, arch, archName, inst, ctx, error))
            return false;
        out.initialState.instances[id] = inst;
        out.attrs[id] = attrs;
        if (attrs.patrol.enabled) out.patrolIds.push_back(id);
        if (attrs.solid)          out.solidIds.push_back(id);

        // What walking up to it does: "talk" opens a conversation on the
        // interact action; "pickup" puts it in the inventory on touch.
        const JsonValue* talk = merged->find("talk");
        const JsonValue* pickup = merged->find("pickup");
        if (talk || pickup) {
            Interactable what;
            what.name = merged->find("name") ? merged->find("name")->string()
                      : merged->find("label") ? merged->find("label")->string() : name;
            if (talk) {
                what.talk = talk->string();
                out.talkIds.push_back(id);
            }
            if (pickup) {
                // "pickup": "herb"  or  "pickup": {"item": "herb", "count": 3}
                if (pickup->isObject()) {
                    what.item = pickup->find("item") ? pickup->find("item")->string() : "";
                    if (const JsonValue* c = pickup->find("count"))
                        what.count = static_cast<int>(c->number(1));
                } else {
                    what.item = pickup->string();
                }
                if (what.item.empty()) return failCtx(error, ctx + ".pickup", "missing item id");
                if (what.count < 1)    return failCtx(error, ctx + ".pickup.count", "must be at least 1");
                out.pickupIds.push_back(id);
            }
            out.interactables[id] = std::move(what);
        }

        // Label: billboarded glyphs under the feet, sharing the owner's motion.
        if (const JsonValue* lab = merged->find("label")) {
            Vec4 tint = {1, 1, 1, 1};
            if (!readVec4(merged->find("labelTint"), tint, ctx + ".labelTint", error))
                return false;
            const LabelStyle& st = out.labelStyle;
            Vec3 anchor = inst.pos + Vec3{0.0f, attrs.labelDy, 0.0f};
            std::vector<Instance> glyphs = makeLabel(
                lab->string(), anchor, st.height, st.advance, st.drop, st.sheet, tint);
            std::vector<EntityId>& ids = out.labels[id];
            for (Instance& g : glyphs) {
                setMotion(g, inst.vel, inst.accel, inst.motionStart);
                out.initialState.instances[nextId] = g;
                ids.push_back(nextId++);
            }
        }
        return true;
    };

    if (const JsonValue* ents = root.find("entities")) {
        if (ents->isArray()) {
            for (size_t i = 0; i < ents->arr.size(); ++i)
                if (!addEntity(ents->arr[i], ctxIndex("entities", i))) return false;
        }
    }

    // Entities the terrain map asks for: one per "spawn" cell, standing at the
    // cell centre, named "<archetype>@<x>,<z>".
    for (const TerrainSpawn& sp : spawns) {
        char name[96];
        std::snprintf(name, sizeof(name), "%s@%d,%d", sp.archetype.c_str(), sp.x, sp.z);
        JsonValue e;
        e.type = JsonValue::Type::Object;
        auto str = [](const std::string& v) {
            JsonValue j; j.type = JsonValue::Type::String; j.strVal = v; return j;
        };
        auto num = [](double v) {
            JsonValue j; j.type = JsonValue::Type::Number; j.numVal = v; return j;
        };
        JsonValue pos;
        pos.type = JsonValue::Type::Array;
        pos.arr = {num(sp.x + 0.5), num(0.0), num(sp.z + 0.5)};
        e.obj.emplace_back("id", str(name));
        e.obj.emplace_back("archetype", str(sp.archetype));
        e.obj.emplace_back("pos", pos);
        if (!addEntity(e, std::string("terrain spawn '") + name + "'")) return false;
    }

    // Camera rig — the follow camera with its two views (engine/camera_rig.h).
    if (const JsonValue* rig = root.find("cameraRig")) {
        out.rig.enabled = true;
        if (!readRigView(rig->find("topdown"), out.rig.topdown, "cameraRig.topdown", error)) return false;
        if (!readRigView(rig->find("dungeon"), out.rig.dungeon, "cameraRig.dungeon", error)) return false;
        if (const JsonValue* v = rig->find("view")) {
            const std::string view = v->string();
            if      (view == "topdown") out.rig.startView = RIG_TOPDOWN;
            else if (view == "dungeon") out.rig.startView = RIG_DUNGEON;
            else return failCtx(error, "cameraRig.view", "unknown view '" + view + "'");
        }
        if (const JsonValue* f = rig->find("facing")) {
            const std::string facing = f->string();
            if      (facing == "north") out.rig.startQuarter = 0;
            else if (facing == "east")  out.rig.startQuarter = 1;
            else if (facing == "south") out.rig.startQuarter = 2;
            else if (facing == "west")  out.rig.startQuarter = 3;
            else return failCtx(error, "cameraRig.facing", "unknown facing '" + facing + "'");
        }
        if (const JsonValue* f = rig->find("follow")) {
            if (!checkEntityRef(out, f->string(), "follow", "cameraRig", error)) return false;
            out.rigFollow = out.idOf(f->string());
        } else {
            // Default: the first controlled entity.
            for (const auto& kv : out.attrs)
                if (kv.second.controlled && (out.rigFollow == 0 || kv.first < out.rigFollow))
                    out.rigFollow = kv.first;
            if (out.rigFollow == 0)
                return failCtx(error, "cameraRig", "needs 'follow' or a controlled entity");
        }
    }

    // Sky backdrop: a vertical gradient (zenith → horizon → ground).
    if (const JsonValue* sky = root.find("sky")) {
        Sky& s = out.initialState.sky;
        s.enabled = true;
        if (!readVec3(sky->find("zenith"),  s.zenith,  "sky.zenith",  error)) return false;
        if (!readVec3(sky->find("horizon"), s.horizon, "sky.horizon", error)) return false;
        if (!readVec3(sky->find("ground"),  s.ground,  "sky.ground",  error)) return false;
    }

    // Conversations — after archetypes (speakers) and entities (who talks).
    if (const JsonValue* dlgs = root.find("dialogues")) {
        if (!dlgs->isObject()) return failCtx(error, "dialogues", "must be an object");
        if (!out.ui.enabled)
            return failCtx(error, "dialogues", "need a 'ui' block (the window and font sheets)");
        for (const auto& kv : dlgs->obj) {
            const std::string ctx = "dialogues." + kv.first;
            if (!kv.second.isArray() || kv.second.arr.empty())
                return failCtx(error, ctx, "must be a non-empty array of lines");
            Dialogue dlg;
            for (size_t i = 0; i < kv.second.arr.size(); ++i) {
                DialogueLine line;
                if (!readDialogueLine(kv.second.arr[i], out, line, ctxIndex(ctx, i), error))
                    return false;
                dlg.lines.push_back(std::move(line));
            }
            out.dialogues[kv.first] = std::move(dlg);
        }
    }
    for (EntityId id : out.talkIds) {
        const Interactable& what = out.interactables[id];
        if (!out.dialogues.count(what.talk))
            return failCtx(error, "entity '" + what.name + "'",
                           "'talk' references unknown dialogue '" + what.talk + "'");
    }
    if (!out.pickupIds.empty() && !out.ui.enabled)
        std::fprintf(stderr, "warning: %s: pickups without a 'ui' block — the inventory "
                             "will not be shown\n", path);

    // Events — parsed after entities so triggers/actions can validate every
    // entity reference against the scene.
    if (const JsonValue* evs = root.find("events")) {
        if (evs->isArray()) {
            for (size_t i = 0; i < evs->arr.size(); ++i) {
                const JsonValue& ev = evs->arr[i];
                const std::string ctx = ctxIndex("events", i);
                Event event;
                if (const JsonValue* t = ev.find("trigger")) {
                    if (!readTrigger(*t, out, event.trigger, ctx + ".trigger", error))
                        return false;
                }
                if (const JsonValue* c = ev.find("condition")) {
                    if (!readCondition(*c, event.condition, ctx + ".condition", error))
                        return false;
                }
                if (const JsonValue* once = ev.find("once")) event.once = once->boolean(true);
                if (const JsonValue* acts = ev.find("actions")) {
                    if (acts->isArray()) {
                        for (size_t j = 0; j < acts->arr.size(); ++j) {
                            Action action;
                            if (!readAction(acts->arr[j], out, action,
                                            ctxIndex(ctx + ".actions", j), error))
                                return false;
                            event.actions.push_back(std::move(action));
                        }
                    }
                }
                out.events.push_back(std::move(event));
            }
        }
    }

    // Controls — key name → abstract action. Key names are lowercase and
    // device-agnostic; the engine resolves SDL keys to these names each frame.
    if (const JsonValue* controls = root.find("controls")) {
        if (const JsonValue* b = controls->find("bindings")) {
            if (b->isObject())
                for (const auto& kv : b->obj)
                    out.bindings[kv.first] = kv.second.string();
        }
    }

    // Optional dialogueText config: when present, the `dialogue` action also
    // spawns the line's text at this position for a few seconds (events.cpp).
    if (const JsonValue* dt = root.find("dialogueText")) {
        out.dialogueText.enabled = true;
        if (!readVec3(dt->find("pos"), out.dialogueText.pos,
                      "dialogueText.pos", error)) return false;
        if (!readVec2(dt->find("charSize"), out.dialogueText.charSize,
                      "dialogueText.charSize", error)) return false;
        if (!readVec4(dt->find("tint"), out.dialogueText.tint,
                      "dialogueText.tint", error)) return false;
        if (const JsonValue* sh = dt->find("sheet"))
            out.dialogueText.sheet = static_cast<int>(sh->number(0));
    }

    return true;
}

} // namespace cv
