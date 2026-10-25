/**
 * @file PhysicsSystem.h
 * @author Jackey Cen
 * @brief Steps the World's PhysicsWorld and keeps RigidBody and Transform in sync.
 *
 * Registered in SystemPhase::FixedUpdate, so it runs once per fixed substep
 * with context.dt as the step - and before PostUpdate, where
 * TransformPropagateSystem turns the Transforms it wrote into world matrices.
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
    class PhysicsWorld;

    class PhysicsSystem final : public ISystem
    {
    public:
        void OnStart(SystemContext &context) override;
        void OnUpdate(SystemContext &context) override;

        // RigidBody's OnAdd: clears the runtime block, then creates the body while the simulation is live
        static void OnRigidBodyAdded(World &world, Entity entity);
        // RigidBody's OnRemove: frees its Jolt body
        static void OnRigidBodyRemoved(World &world, Entity entity);

        // PlayBegin: bodies for the RigidBodies already in the scene, then the simulation goes live
        static void OnPlayBegin(World &world);
        // PlayEnd: the simulation stops being live before the restore adds its entities
        static void OnPlayEnd(World &world);

    private:
        // New bodies and Transform edits reach Jolt before the step; Jolt's poses come back after it.
        void PushTransforms(World &world, PhysicsWorld &physics);
        void PullPoses(PhysicsWorld &physics);

        Query<Transform, RigidBody> *mQuery = nullptr;
    };
}
