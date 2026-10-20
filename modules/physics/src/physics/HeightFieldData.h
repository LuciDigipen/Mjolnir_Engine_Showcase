/**
 * @file HeightFieldData.h
 * @author sumin.park
 * @brief The Jolt shape behind a SharedHeightField, for BuildHeightField and PhysicsWorld.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <physics/HeightField.h>

#include "JoltRuntime.h"

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <optional>

namespace mir
{
    struct SharedHeightField::Data
    {
        // First, so Jolt outlives the shape.
        detail::JoltRuntimeRef mRuntime;
        JPH::RefConst<JPH::Shape> mShape;
        std::optional<HeightFieldInfo> mInfo;
    };
}
