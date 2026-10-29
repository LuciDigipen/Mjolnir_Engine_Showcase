/**
 * @file ColliderDebugSystem.cpp
 * @author Rahul Nair
 * @brief Outlines every collider into DebugLines while a view asks for them.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/ColliderDebugSystem.h>

#include "BodyPose.h"

#include <physics/ColliderLines.h>
#include <physics/PhysicsWorld.h>

#include <core/debug/DebugLines.h>
#include <core/ecs/World.h>

namespace mir
{
    void ColliderDebugSystem::OnStart(SystemContext &context)
    {
        mQuery = &context.world.GetOrCreateQuery<const Transform, const RigidBody>();
    }

    void ColliderDebugSystem::OnUpdate(SystemContext &context)
    {
        DebugLines *lines = context.world.TryResource<DebugLines>();
        if (lines == nullptr)
            return;

        lines->mColliders.clear();
        if (!lines->mCollidersWanted)
            return;

        std::vector<DebugLineVertex> &out = lines->mColliders;
        if (const PhysicsWorld *physics = context.world.TryResource<PhysicsWorld>())
            physics->ForEachCollider([&](const ColliderView &view) { AppendColliderLines(view, out); });

        // a RigidBody with no body yet draws what PlayBegin will build for it
        mQuery->ForEach([&](Entity entity, const Transform &, const RigidBody &rigidBody)
            {
                if (rigidBody.runtime.body.IsValid())
                    return;
                const std::optional<ColliderView> view =
                    ColliderViewOf(detail::ShapeDescOf(rigidBody, detail::PoseOf(context.world, entity)));
                if (view)
                    AppendColliderLines(*view, out);
            });
    }
}
