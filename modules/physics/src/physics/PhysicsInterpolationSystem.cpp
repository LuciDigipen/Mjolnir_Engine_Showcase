/**
 * @file PhysicsInterpolationSystem.cpp
 * @author Quy Dong Ta
 * @brief The per-frame step-pose blend into Transform.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/PhysicsInterpolationSystem.h>

#include <core/ecs/EngineMode.h>
#include <core/ecs/World.h>

#include <glm/common.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>

namespace mir
{
    void PhysicsInterpolationSystem::OnStart(SystemContext &context)
    {
        mQuery = &context.world.GetOrCreateQuery<Transform, RigidBody>();
    }

    void PhysicsInterpolationSystem::OnUpdate(SystemContext &context)
    {
        if (!IsSimulating(context.world))
            return;

        const float alpha = std::clamp(context.fixedAlpha, 0.0f, 1.0f);

        mQuery->ForEach(
            [&](Entity, Transform &transform, RigidBody &rigidBody)
            {
                if (!rigidBody.interpolate || !rigidBody.body.IsValid() ||
                    rigidBody.motion != PhysicsMotion::Dynamic)
                    return;

                if (transform.Version() != rigidBody.transformVersion)
                    return;

                const WorldVec3 position =
                    glm::mix(rigidBody.previousPosition, rigidBody.currentPosition, static_cast<WorldReal>(alpha));
                const glm::quat rotation = glm::slerp(rigidBody.previousRotation, rigidBody.currentRotation, alpha);

                if (position != transform.Position())
                    transform.SetPosition(position);
                if (rotation != transform.Rotation())
                    transform.SetRotation(rotation);

                rigidBody.transformVersion = transform.Version();
            });
    }
}
