/**
 * @file CameraFollowSystem.cpp
 * @author Quy Dong Ta
 * @brief Exponential follow toward a target, plus an optional look-at.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "renderer/CameraFollowSystem.h"

#include <core/ecs/EngineMode.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/WorldTransform.h>
#include <core/log/Log.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace mir
{
    namespace
    {
        constexpr float kMinLookDistance = 1e-4f;


        constexpr float kMaxLookDotUp = 0.9999f;
    }

    WorldVec3 CameraFollowGoal(const CameraFollow &follow, const WorldVec3 &targetPosition,
        const glm::quat &targetRotation)
    {
        const glm::vec3 offset = follow.mOffsetInTargetSpace ? targetRotation * follow.mOffset : follow.mOffset;
        return targetPosition + WorldVec3(offset);
    }

    FollowPose StepCameraFollow(const CameraFollow &follow, const FollowPose &camera, const WorldVec3 &goal,
        const WorldVec3 &lookAt, float dt, bool snap)
    {
        const WorldVec3 gap = goal - camera.position;
        const bool jump = follow.mSnapDistance > 0.0f && glm::length(gap) > static_cast<WorldReal>(follow.mSnapDistance);


        const float blend = (snap || jump)
            ? 1.0f
            : 1.0f - std::exp(-std::max(follow.mStiffness, 0.0f) * std::max(dt, 0.0f));

        FollowPose next = camera;
        next.position = camera.position + gap * static_cast<WorldReal>(blend);

        if (follow.mLookAtTarget)
        {
            const glm::vec3 toTarget(lookAt - next.position);
            const float distance = glm::length(toTarget);
            if (distance > kMinLookDistance)
            {
                const glm::vec3 forward = toTarget / distance;
                if (std::abs(forward.y) < kMaxLookDotUp)
                {
                    const glm::quat facing = glm::quatLookAt(forward, glm::vec3(0.0f, 1.0f, 0.0f));
                    next.rotation = glm::normalize(glm::slerp(camera.rotation, facing, blend));
                }
            }
        }

        return next;
    }

    void CameraFollowSystem::OnStart(SystemContext &context)
    {
        mQuery = &context.world.GetOrCreateQuery<Transform, CameraFollow>();
    }

    void CameraFollowSystem::OnUpdate(SystemContext &context)
    {
        World &world = context.world;
        if (!IsSimulating(world))
            return;

        mQuery->ForEach(
            [&](Entity entity, Transform &transform, CameraFollow &follow)
            {
                const Entity target = follow.mTarget;
                if (target.IsNull() || target == entity || !world.IsAlive(target) || !world.Has<WorldTransform>(target))
                    return;

                if (!ParentOf(world, entity).IsNull())
                {
                    if (!mWarnedParented)
                    {
                        MIR_LOG_WARN("CameraFollowSystem: entity {} has a parent; CameraFollow only moves root "
                                     "entities, so it will not follow",
                            entity.mIndex);
                        mWarnedParented = true;
                    }
                    return;
                }

                const WorldPose targetPose = ResolveWorld(world, target);
                glm::quat targetRotation{};
                glm::vec3 targetScale{};
                DecomposeRotationScale(targetPose.linear, targetRotation, targetScale);

                const WorldVec3 targetPosition = targetPose.position.Meters();
                const FollowPose camera{ transform.Position(), transform.Rotation() };
                const FollowPose next =
                    StepCameraFollow(follow, camera, CameraFollowGoal(follow, targetPosition, targetRotation),
                        targetPosition, context.dt, !follow.mPlaced);
                follow.mPlaced = true;

                if (next.position != camera.position)
                    transform.SetPosition(next.position);
                if (next.rotation != camera.rotation)
                    transform.SetRotation(next.rotation);


                ResolveWorld(world, entity);
            });
    }
}
