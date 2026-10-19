/**
 * @file CameraControllerSystem.h
 * @author Quy Dong Ta
 * @brief Moves every CameraController entity, by its target or by free-fly input, each frame.
 *
 * Registered in SystemPhase::PostUpdate right after TransformPropagateSystem.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "camera/components/CameraController.h"

#include <core/ecs/Query.h>
#include <core/ecs/System.h>
#include <core/ecs/components/Transform.h>
#include <core/math/WorldReal.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>

namespace mir
{
    struct CameraPose
    {
        WorldVec3 position{ 0 };
        glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    };

    struct FreeFlyInput
    {
        glm::vec2 move{ 0.0f };
        float vertical = 0.0f;
        glm::vec2 lookPixels{ 0.0f };
        glm::vec2 lookStick{ 0.0f };
        bool fast = false;
    };

    WorldVec3 CameraFollowGoal(const CameraController &controller, const WorldVec3 &targetPosition, const glm::quat &targetRotation);

    CameraPose StepCameraFollow(const CameraController &controller, const CameraPose &camera, const WorldVec3 &goal, const WorldVec3 &lookAt, float dt, bool snap);

    void SeedFreeFly(CameraController &controller, const glm::quat &rotation);

    CameraPose StepFreeFly(CameraController &controller, const WorldVec3 &position, const FreeFlyInput &input, float dt);

    class CameraControllerSystem final : public ISystem
    {
        
    public:
        void OnStart(SystemContext &context) override;
        void OnUpdate(SystemContext &context) override;

    private:
        Query<Transform, CameraController> *mQuery = nullptr;
        bool mWarnedParented = false;
    };
}
