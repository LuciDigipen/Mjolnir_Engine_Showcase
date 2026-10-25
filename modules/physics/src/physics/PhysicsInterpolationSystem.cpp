/**
 * @file PhysicsInterpolationSystem.cpp
 * @author Quy Dong Ta
 * @brief The per-frame step-pose blend into Transform.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/PhysicsInterpolationSystem.h>

#include "BodyPose.h"

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

        World &world = context.world;
        mQuery->ForEach(
            [&](Entity entity, Transform &transform, RigidBody &rigidBody)
            {
                if (!rigidBody.interpolate || !HasMovingBody(rigidBody))
                    return;

                // Moved by something else since physics last placed it: that write stands until the next push.
                const detail::BodyPose pose = detail::PoseOf(world, entity);
                if (detail::PositionWritten(rigidBody, pose) || detail::RotationWritten(rigidBody, pose))
                    return;

                const WorldVec3 position =
                    glm::mix(rigidBody.runtime.previousPosition, rigidBody.runtime.currentPosition, static_cast<WorldReal>(alpha));
                const glm::quat rotation = glm::slerp(rigidBody.runtime.previousRotation, rigidBody.runtime.currentRotation, alpha);

                const std::optional<WorldPosition> blended = WorldPosition::FromMeters(position);
                if (!blended || !detail::WriteWorldPose(world, entity, transform, *blended, rotation))
                    return;

                detail::Stamp(rigidBody, detail::PoseOf(world, entity));
            });
    }
}
