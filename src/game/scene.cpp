// scene.cpp — parse a JSON scene file into a Scene (WorldState + events).
#include "scene.h"
#include "json.h"

#include <cstdio>

namespace cv {

namespace {

Vec3 readVec3(const JsonValue* v, Vec3 fallback) {
    if (!v || !v->isArray()) return fallback;
    return { v->at(0, fallback.x), v->at(1, fallback.y), v->at(2, fallback.z) };
}

Vec2 readVec2(const JsonValue* v, Vec2 fallback) {
    if (!v || !v->isArray()) return fallback;
    return { v->at(0, fallback.x), v->at(1, fallback.y) };
}

Camera readCamera(const JsonValue& c) {
    Camera cam;
    if (const JsonValue* p = c.find("projection"))
        cam.projection = (p->string() == "orthographic")
            ? Projection::Orthographic : Projection::Perspective;
    cam.position = readVec3(c.find("position"), cam.position);
    cam.target   = readVec3(c.find("target"),   cam.target);
    if (const JsonValue* up = c.find("up")) cam.up = readVec3(up, cam.up);
    if (const JsonValue* fov = c.find("fovY")) cam.fovYRadians = static_cast<float>(fov->number(cam.fovYRadians));
    if (const JsonValue* oh = c.find("orthoHalfHeight"))
        cam.orthoHalfHeight = static_cast<float>(oh->number(cam.orthoHalfHeight));
    return cam;
}

Instance readEntity(const JsonValue& e) {
    Vec3 pos   = readVec3(e.find("pos"), {0, 0, 0});
    Vec2 scale = readVec2(e.find("scale"), {1, 1});
    bool bb = true;
    if (const JsonValue* b = e.find("billboard")) bb = b->boolean(true);

    Instance inst = bb ? makeBillboard(pos, scale) : makeSprite(pos, scale);
    if (!bb && e.find("rotation"))
        inst.rotation = static_cast<float>(e.find("rotation")->number());

    if (const JsonValue* vel = e.find("vel"))
        inst.vel = readVec3(vel, {0, 0, 0});
    if (const JsonValue* acc = e.find("accel"))
        inst.accel = readVec3(acc, {0, 0, 0});

    if (const JsonValue* a = e.find("anim")) {
        int   first = static_cast<int>(a->find("first") ? a->find("first")->number() : 0);
        int   count = static_cast<int>(a->find("count") ? a->find("count")->number() : 1);
        float fps   = a->find("fps") ? static_cast<float>(a->find("fps")->number()) : 0.0f;
        float start = a->find("start") ? static_cast<float>(a->find("start")->number()) : 0.0f;
        setAnimation(inst, first, count, fps, start);
    }
    return inst;
}

Trigger readTrigger(const JsonValue& t) {
    Trigger tr;
    std::string type = t.find("type") ? t.find("type")->string() : "start";
    if (type == "proximity") {
        tr.type = Trigger::Type::Proximity;
        tr.entity = t.find("entity") ? t.find("entity")->string() : "";
        tr.target = t.find("target") ? t.find("target")->string() : "";
        tr.radius = t.find("radius") ? static_cast<float>(t.find("radius")->number()) : 1.0f;
    } else {
        tr.type = Trigger::Type::Start;
    }
    return tr;
}

Action readAction(const JsonValue& a) {
    Action ac;
    std::string type = a.find("type") ? a.find("type")->string() : "dialogue";
    if (type == "set_flag") {
        ac.type  = Action::Type::SetFlag;
        ac.flag  = a.find("flag") ? a.find("flag")->string() : "";
        ac.value = a.find("value") ? a.find("value")->boolean(true) : true;
    } else if (type == "set_anim") {
        ac.type   = Action::Type::SetAnim;
        ac.entity = a.find("entity") ? a.find("entity")->string() : "";
        ac.first  = static_cast<int>(a.find("first") ? a.find("first")->number() : 0);
        ac.count  = static_cast<int>(a.find("count") ? a.find("count")->number() : 1);
        ac.fps    = a.find("fps") ? static_cast<float>(a.find("fps")->number()) : 0.0f;
    } else if (type == "set_motion") {
        ac.type   = Action::Type::SetMotion;
        ac.entity = a.find("entity") ? a.find("entity")->string() : "";
        ac.vel    = readVec3(a.find("vel"),   {0, 0, 0});
        ac.accel  = readVec3(a.find("accel"), {0, 0, 0});
    } else if (type == "remove") {
        ac.type   = Action::Type::Remove;
        ac.entity = a.find("entity") ? a.find("entity")->string() : "";
    } else {
        ac.type = Action::Type::Dialogue;
        ac.id   = a.find("id") ? a.find("id")->string() : "";
    }
    return ac;
}

} // namespace

bool loadScene(const char* path, Scene& out, std::string& error) {
    std::string text;
    if (!readFile(path, text)) { error = std::string("cannot read scene file: ") + path; return false; }

    JsonValue root;
    if (!JsonParser::parse(text, root, error)) return false;
    if (!root.isObject()) { error = "scene root must be a JSON object"; return false; }

    // Sheet
    if (const JsonValue* s = root.find("sheet")) {
        if (const JsonValue* p = s->find("path")) out.sheet.path = p->string(out.sheet.path);
        if (const JsonValue* c = s->find("cols")) out.sheet.cols = static_cast<int>(c->number(out.sheet.cols));
        if (const JsonValue* r = s->find("rows")) out.sheet.rows = static_cast<int>(r->number(out.sheet.rows));
    }

    // Cameras
    if (const JsonValue* cams = root.find("cameras")) {
        if (cams->isArray())
            for (const JsonValue& c : cams->arr)
                out.initialState.cameras.push_back(readCamera(c));
    }
    if (out.initialState.cameras.empty())
        out.initialState.cameras.push_back(Camera{});  // sane default
    if (const JsonValue* ac = root.find("activeCamera"))
        out.initialState.activeCamera = static_cast<int>(ac->number(0));

    // Entities — assign numeric ids starting at 1, record name → id.
    EntityId nextId = 1;
    if (const JsonValue* ents = root.find("entities")) {
        if (ents->isArray()) {
            for (const JsonValue& e : ents->arr) {
                std::string name = e.find("id") ? e.find("id")->string() : "";
                EntityId id = nextId++;
                if (!name.empty()) out.nameToId[name] = id;
                out.initialState.instances[id] = readEntity(e);
            }
        }
    }

    // Events
    if (const JsonValue* evs = root.find("events")) {
        if (evs->isArray()) {
            for (const JsonValue& ev : evs->arr) {
                Event event;
                if (const JsonValue* t = ev.find("trigger")) event.trigger = readTrigger(*t);
                if (const JsonValue* c = ev.find("condition")) {
                    event.condition.present = true;
                    event.condition.flag  = c->find("flag") ? c->find("flag")->string() : "";
                    event.condition.value = c->find("value") ? c->find("value")->boolean(true) : true;
                }
                if (const JsonValue* once = ev.find("once")) event.once = once->boolean(true);
                if (const JsonValue* acts = ev.find("actions")) {
                    if (acts->isArray())
                        for (const JsonValue& a : acts->arr)
                            event.actions.push_back(readAction(a));
                }
                out.events.push_back(std::move(event));
            }
        }
    }

    return true;
}

} // namespace cv
