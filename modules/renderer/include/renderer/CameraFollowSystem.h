/**
 * @file CameraFollowSystem.h
 * @author Quy Dong Ta
 * @brief Moves every CameraFollow entity toward its target each frame.
 *
 * Registered in SystemPhase::PostUpdate *after* TransformPropagateSystem, so a
 * target's WorldTransform is this frame's - including a RigidBody target that
 * PhysicsInterpolationSystem blended just before propagation. The camera's own
 * WorldTransform is refreshed right after its Transform is written, since
 * propagation has already run; RenderSystem then draws it where it is now
 * rather than a frame behind.
 *
 * Runs only while simulating: in Edit the scene keeps the authored pose.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "renderer/components/CameraFollow.h"

#include <core/ecs/Query.h>
#include <core/ecs/System.h>
#include <core/ecs/components/Transform.h>
#include <core/math/WorldReal.h>

#include <glm/gtc/quaternion.hpp>

namespace mir
{

    struct FollowPose
    {
        WorldVec3 position{ 0 };
        glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    };


    WorldVec3 CameraFollowGoal(const CameraFollow &follow, const WorldVec3 &targetPosition, const glm::quat &targetRotation);


    FollowPose StepCameraFollow(const CameraFollow &follow, const FollowPose &camera, const WorldVec3 &goal, const WorldVec3 &lookAt, float dt, bool snap);

    class CameraFollowSystem final : public ISystem
    {
        
    public:
        void OnStart(SystemContext &context) override;
        void OnUpdate(SystemContext &context) override;

    private:
        Query<Transform, CameraFollow> *mQuery = nullptr;
        bool mWarnedParented = false;
    };
}
