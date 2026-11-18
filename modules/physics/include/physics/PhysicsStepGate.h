/**
 * @file PhysicsStepGate.h
 * @author Jackey Cen
 * @brief Lets another module act on bodies before and after each fixed step.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/World.h>

#include <vector>

namespace mir
{
    class PhysicsWorld;

    class PhysicsStepGate
    {
      public:
        virtual ~PhysicsStepGate() = default;

        // Once per fixed step, after new bodies and Transform edits reach the PhysicsWorld and before it steps.
        virtual void BeforeStep(World &world, PhysicsWorld &physics, float dt) = 0;

        // Once per fixed step, after it and its write-back, so both the body and the Transform show the step.
        virtual void AfterStep(World &, PhysicsWorld &, float) {}
    };

    // World resource; a gate removes itself before it goes.
    struct PhysicsStepGates
    {
        std::vector<PhysicsStepGate *> mGates;
    };
} // namespace mir
