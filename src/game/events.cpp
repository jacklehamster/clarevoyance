// events.cpp — EventSystem implementation.
#include "events.h"
#include "scene.h"

#include <cstdio>
#include <cmath>

namespace cv {

void EventSystem::init(const Scene& scene) {
    flags_.clear();
    entities_ = scene.initialState.instances;  // working copy for animation swaps
}

bool EventSystem::conditionPasses(const Condition& c) const {
    if (!c.present) return true;
    return flag(c.flag) == c.value;
}

void EventSystem::runActions(Scene& scene, const std::vector<Action>& actions, Diff& out) {
    for (const Action& a : actions) {
        switch (a.type) {
            case Action::Type::Dialogue:
                // Engine-agnostic: a sink/UI can grep this prefix. For now it logs.
                std::printf("CV_DIALOGUE: %s\n", a.id.c_str());
                break;

            case Action::Type::SetFlag:
                flags_[a.flag] = a.value;
                break;

            case Action::Type::SetAnim: {
                EntityId id = scene.idOf(a.entity);
                if (id == 0) break;
                auto it = entities_.find(id);
                if (it == entities_.end()) break;
                setAnimation(it->second, a.first, a.count, a.fps, 0.0f);
                out.upserts.emplace_back(id, it->second);
                break;
            }

            case Action::Type::Remove: {
                EntityId id = scene.idOf(a.entity);
                if (id == 0) break;
                entities_.erase(id);
                out.removals.push_back(id);
                break;
            }
        }
    }
}

void EventSystem::update(Scene& scene,
                         const std::unordered_map<EntityId, Vec3>& positions,
                         Diff& out) {
    for (Event& e : scene.events) {
        if (e.fired && e.once) continue;
        if (!conditionPasses(e.condition)) continue;

        bool triggered = false;
        switch (e.trigger.type) {
            case Trigger::Type::Start:
                triggered = true;
                break;

            case Trigger::Type::Proximity: {
                EntityId a = scene.idOf(e.trigger.entity);
                EntityId b = scene.idOf(e.trigger.target);
                auto pa = positions.find(a);
                auto pb = positions.find(b);
                if (a == 0 || b == 0 || pa == positions.end() || pb == positions.end())
                    break;
                Vec3 d = pa->second - pb->second;
                float dist2 = dot(d, d);
                float r = e.trigger.radius;
                triggered = dist2 <= r * r;
                break;
            }
        }

        if (triggered) {
            runActions(scene, e.actions, out);
            e.fired = true;
        }
    }
}

} // namespace cv
