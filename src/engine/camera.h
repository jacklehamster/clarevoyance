// camera.h — a view onto the world. 3D perspective or 2D orthographic.
//
// A WorldState can hold several cameras and switch the active one freely, which
// is how dungeon rooms will flip to an overhead view later. The camera also
// exposes its right/up basis so the renderer can billboard sprites toward it.
#pragma once

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
        if (projection == Projection::Orthographic) {
            float hh = orthoHalfHeight;
            float hw = hh * aspect;
            return ortho(-hw, hw, -hh, hh, nearPlane, farPlane);
        }
        return perspective(fovYRadians, aspect, nearPlane, farPlane);
    }

    Mat4 viewProjection() const { return mul(projectionMatrix(), view()); }

    // Camera-space right/up in world coordinates, for billboarding.
    Vec3 right() const { return normalize(cross(normalize(target - position), up)); }
    Vec3 trueUp() const {
        Vec3 fwd = normalize(target - position);
        return normalize(cross(right(), fwd));
    }
};

} // namespace cv
