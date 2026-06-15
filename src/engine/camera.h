// camera.h — a view onto the world. 3D perspective or 2D orthographic.
//
// A WorldState can hold several cameras and switch the active one freely, which
// is how dungeon rooms will flip to an overhead view later. The camera also
// exposes its right/up basis so the renderer can billboard sprites toward it.
#pragma once

#include <cmath>
#include "mathx.h"

namespace cv {

enum class Projection { Perspective, Orthographic };

struct Camera {
    Projection projection = Projection::Perspective;

    Vec3 position = {0.0f, 0.0f, 5.0f};
    Vec3 target = {0.0f, 0.0f, 0.0f};
    Vec3 up = {0.0f, 1.0f, 0.0f};

    // Perspective params.
    float fovYRadians = 1.0472f; // 60 degrees
    float aspect = 16.0f / 9.0f;

    // Orthographic params (half-height of the view volume; width = halfHeight * aspect).
    float orthoHalfHeight = 5.0f;

    float nearPlane = 0.1f;
    float farPlane = 100.0f;

    Mat4 view() const { return lookAt(position, target, up); }

    Mat4 projectionMatrix() const {
        float safeAspect = (aspect > 0.0f) ? aspect : 1.0f;
        float safeNear   = (nearPlane > 0.0f) ? nearPlane : 0.01f;
        float safeFar    = (farPlane > safeNear) ? farPlane : (safeNear + 100.0f);
        if (projection == Projection::Orthographic) {
            float hh = (orthoHalfHeight > 0.0f) ? orthoHalfHeight : 1.0f;
            float hw = hh * safeAspect;
            return ortho(-hw, hw, -hh, hh, safeNear, safeFar);
        }
        return perspective(fovYRadians, safeAspect, safeNear, safeFar);
    }

    Mat4 viewProjection() const { return mul(projectionMatrix(), view()); }

    // Camera-space right/up in world coordinates, for billboarding.
    // Falls back to the Z axis when forward is nearly parallel to up (looking straight down).
    Vec3 right() const {
        Vec3 fwd = normalize(target - position);
        Vec3 upN = normalize(up);
        if (std::fabs(dot(fwd, upN)) > 0.999f)
            upN = {0.0f, 0.0f, 1.0f};
        return normalize(cross(fwd, upN));
    }
    Vec3 trueUp() const {
        Vec3 fwd = normalize(target - position);
        return normalize(cross(right(), fwd));
    }
};

} // namespace cv
