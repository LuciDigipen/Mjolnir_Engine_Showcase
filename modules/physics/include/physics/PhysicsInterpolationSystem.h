/**
 * @file PhysicsInterpolationSystem.h
 * @author Quy Dong Ta
 * @brief Draws interpolating RigidBodies between their last two fixed steps.
 *
 * PhysicsSystem writes a body's pose into Transform once per fixed step, so on
 * a display faster than the step the body - and any camera following it -
 * visibly jumps. For each Dynamic body with RigidBody::interpolate set, this
 * writes the previous-to-current step blend at SystemContext::fixedAlpha
 * instead, and stamps the pose it wrote so PhysicsSystem does not read that
 * write back as a teleport.
 *
 * Registered in SystemPhase::PostUpdate, before Propagate, so
 * the blended Transform is what this frame's world poses are built from.
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
    class PhysicsInterpolationSystem final : public ISystem
    {
    public:
        void OnStart(SystemContext &context) override;
        void OnUpdate(SystemContext &context) override;

    private:
        Query<Transform, RigidBody> *mQuery = nullptr;
    };
}
