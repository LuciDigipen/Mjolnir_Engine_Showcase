/**
 * @file ColliderDebugSystem.h
 * @author Rahul Nair
 * @brief Outlines every collider into DebugLines while a view asks for them.
 *
 * In Play every RigidBody has a Jolt body, so the lines are the shapes Jolt
 * holds at their simulated poses. In Edit no RigidBody has a body yet, so the
 * lines are the shapes PlayBegin would build, at the entities' poses. Height
 * fields have bodies in either mode.
 *
 * Registered in SystemPhase::LateUpdate, after Propagate and every fixed step,
 * so the renderer draws this frame's poses.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <physics/components/RigidBody.h>

#include <core/ecs/Query.h>
#include <core/ecs/System.h>
#include <core/ecs/components/Transform.h>

namespace mir
{
    class ColliderDebugSystem final : public ISystem
    {
    public:
        void OnStart(SystemContext &context) override;
        void OnUpdate(SystemContext &context) override;

    private:
        Query<const Transform, const RigidBody> *mQuery = nullptr;
    };
}
