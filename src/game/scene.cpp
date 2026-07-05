// scene.cpp — parse a JSON scene file into a Scene (WorldState + events).
//
// The loader is STRICT (MAP_EDITOR spec): unknown trigger/action types, unknown
// condition ops, duplicate entity ids, unresolved entity/archetype/clip
// references, and malformed vectors are hard errors with contextual messages
// (e.g. "events[3].actions[0]: unknown action type 'set_flg'"). Scenes carry a
// top-level "version" (currently 1): missing warns on stderr, newer errors.
#include "scene.h"
#include "json.h"
#include "text.h"

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
    out = inst;
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
    if (!c.find("flag"))
        return failCtx(error, ctx, "condition must have 'flag', 'all', or 'any'");
    cond.kind  = Condition::Kind::Flag;
    cond.flag  = c.find("flag")->string();
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

    // Entities — assign numeric ids starting at 1, record name → id. An entity
    // with an "archetype" field takes the archetype's fields as defaults and
    // overrides them key-by-key. Duplicate ids and unknown archetypes are errors.
    EntityId nextId = 1;
    if (const JsonValue* ents = root.find("entities")) {
        if (ents->isArray()) {
            for (size_t i = 0; i < ents->arr.size(); ++i) {
                const JsonValue& e = ents->arr[i];
                const std::string ctx = ctxIndex("entities", i);
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
                    continue;
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
            }
        }
    }

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
