/**
 * @file CameraControllerSystem.cpp
 * @author Quy Dong Ta
 * @brief Exponential follow with an optional look-at, and input-driven free-fly.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "camera/CameraControllerSystem.h"

#include <core/ecs/EngineMode.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/WorldTransform.h>
#include <core/log/Log.h>
#include <input/DefaultInputMap.h>
#include <input/InputState.h>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

namespace mir
{
    namespace
    {
        constexpr float kMinLookDistance = 1e-4f;
        constexpr float kMaxLookDotUp = 0.9999f;
        constexpr float kMaxPitchDegrees = 89.0f;
        const glm::vec3 kUp{ 0.0f, 1.0f, 0.0f };

        glm::vec3 FreeFlyForward(float yawDegrees, float pitchDegrees)
        {
            const float yaw = glm::radians(yawDegrees);
            const float pitch = glm::radians(pitchDegrees);
            return glm::normalize(
                glm::vec3{ std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch) });
        }

        FreeFlyInput ReadFreeFly(const InputState &input)
        {
            FreeFlyInput frame;
            const ActionValue &move = input.ForGame(kCameraMoveAction);
            frame.move = { move.x, move.y };
            frame.vertical = input.ForGame(kCameraVerticalAction).x;
            if (input.ForGame(kCameraLookHoldAction).down)
            {
                const ActionValue &look = input.ForGame(kCameraLookAction);
                frame.lookPixels = { look.x, look.y };
            }
            const ActionValue &stick = input.ForGame(kCameraLookStickAction);
            frame.lookStick = { stick.x, stick.y };
            frame.fast = input.ForGame(kCameraFastAction).down;
            return frame;
        }
    }

    WorldVec3 CameraFollowGoal(const CameraController &controller, const WorldVec3 &targetPosition, const glm::quat &targetRotation)
    {

        const glm::vec3 offset = controller.mOffsetInTargetSpace ? targetRotation * controller.mOffset : controller.mOffset;
        return targetPosition + WorldVec3(offset);
    }

    CameraPose StepCameraFollow(const CameraController &controller, const CameraPose &camera, const WorldVec3 &goal, const WorldVec3 &lookAt, float dt, bool snap)
    {
        const WorldVec3 gap = goal - camera.position;
        const bool jump = !controller.mReturning && controller.mSnapDistance > 0.0f && glm::length(gap) > static_cast<WorldReal>(controller.mSnapDistance);

        const float blend = (snap || jump) ? 1.0f : 1.0f - std::exp(-std::max(controller.mStiffness, 0.0f) * std::max(dt, 0.0f));

        CameraPose next = camera;
        next.position = camera.position + gap * static_cast<WorldReal>(blend);

        if (controller.mLookAtTarget)
        {
            const glm::vec3 toTarget(lookAt - next.position);
            const float distance = glm::length(toTarget);
            if (distance > kMinLookDistance)
            {
                const glm::vec3 forward = toTarget / distance;
                if (std::abs(forward.y) < kMaxLookDotUp)
                {
                    const glm::quat facing = glm::quatLookAt(forward, kUp);
                    next.rotation = glm::normalize(glm::slerp(camera.rotation, facing, blend));
                }
            }
        }

        return next;
    }

    void SeedFreeFly(CameraController &controller, const glm::quat &rotation)
    {
        const glm::vec3 forward = rotation * glm::vec3(0.0f, 0.0f, -1.0f);
        const float pitch = glm::degrees(std::asin(std::clamp(forward.y, -1.0f, 1.0f)));
        controller.mPitchDegrees = std::clamp(pitch, -kMaxPitchDegrees, kMaxPitchDegrees);

        if (forward.x * forward.x + forward.z * forward.z > kMinLookDistance * kMinLookDistance)
            controller.mYawDegrees = glm::degrees(std::atan2(forward.z, forward.x));
    }

    CameraPose StepFreeFly(CameraController &controller, const WorldVec3 &position, const FreeFlyInput &input, float dt)
    {
        dt = std::max(dt, 0.0f);

        const float stickTurn = controller.mStickLookRate * dt;
        controller.mYawDegrees = std::fmod(controller.mYawDegrees + input.lookPixels.x * controller.mLookSensitivity + input.lookStick.x * stickTurn, 360.0f);
        controller.mPitchDegrees = std::clamp(controller.mPitchDegrees - input.lookPixels.y * controller.mLookSensitivity - input.lookStick.y * stickTurn, -kMaxPitchDegrees, kMaxPitchDegrees);

        const glm::vec3 forward = FreeFlyForward(controller.mYawDegrees, controller.mPitchDegrees);
        const glm::vec3 right = glm::normalize(glm::cross(forward, kUp));
        const glm::vec3 up = glm::cross(right, forward);

        glm::vec2 move = input.move;
        if (glm::length(move) > 1.0f)
            move = glm::normalize(move);

        const float speed = controller.mMoveSpeed * (input.fast ? controller.mFastMultiplier : 1.0f) * dt;
        const glm::vec3 step = (right * move.x - forward * move.y + kUp * input.vertical) * speed;

        return CameraPose{ position + WorldVec3(step), glm::quat_cast(glm::mat3(right, up, -forward)) };
    }

    void CameraControllerSystem::OnStart(SystemContext &context)
    {
        mQuery = &context.world.GetOrCreateQuery<Transform, CameraController>();
    }

    void CameraControllerSystem::OnUpdate(SystemContext &context)
    {

        World &world = context.world;

        if (!IsPlaying(world))
            return;


        const bool simulating = IsSimulating(world);
        const InputState *input = world.TryResource<InputState>();
        const bool toggle = input != nullptr && input->ForGame(kToggleCameraModeAction).justPressed;

        mQuery->ForEach(
            [&](Entity entity, Transform &transform, CameraController &controller)
            {
                if (!ParentOf(world, entity).IsNull())
                {
                    if (!mWarnedParented)
                    {
                        MIR_LOG_WARN("CameraControllerSystem: entity {} has a parent; CameraController only moves " "root entities, so it will not move", entity.mIndex);
                        mWarnedParented = true;
                    }
                    return;
                }

                if (toggle)
                    controller.mMode = controller.mMode == CameraMode::Follow ? CameraMode::FreeFly : CameraMode::Follow;


                if (controller.mMode != controller.mLastMode)
                {
                    if (controller.mMode == CameraMode::FreeFly)
                    {
                        SeedFreeFly(controller, transform.Rotation());
                    }
                    else
                    {
                        controller.mPlaced = true;
                        controller.mReturning = true;
                    }
                    controller.mLastMode = controller.mMode;
                }

                const CameraPose camera{ transform.Position(), transform.Rotation() };
                CameraPose next = camera;

                if (controller.mMode == CameraMode::FreeFly)
                {
                    if (input == nullptr)
                        return;
                    next = StepFreeFly(controller, camera.position, ReadFreeFly(*input), context.dt);
                }
                else
                {
                    const Entity target = controller.mTarget;
                    if (!simulating || target.IsNull() || target == entity || !world.IsAlive(target) ||
                        !world.Has<WorldTransform>(target))
                        return;

                    const WorldPose targetPose = ResolveWorld(world, target);
                    glm::quat targetRotation{};
                    glm::vec3 targetScale{};
                    DecomposeRotationScale(targetPose.linear, targetRotation, targetScale);

                    const WorldVec3 targetPosition = targetPose.position.Meters();
                    const WorldVec3 goal = CameraFollowGoal(controller, targetPosition, targetRotation);
                    next = StepCameraFollow(controller, camera, goal, targetPosition, context.dt, !controller.mPlaced);
                    controller.mPlaced = true;

                    if (controller.mReturning && (controller.mSnapDistance <= 0.0f || glm::length(goal - next.position) <= static_cast<WorldReal>(controller.mSnapDistance)))
                        controller.mReturning = false;
                }

                if (next.position != camera.position)
                    transform.SetPosition(next.position);
                if (next.rotation != camera.rotation)
                    transform.SetRotation(next.rotation);

                ResolveWorld(world, entity);
            });
    }
}
