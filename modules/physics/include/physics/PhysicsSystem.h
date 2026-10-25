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
#include <core/math/WorldCoordinates.h>

#include <glm/gtc/quaternion.hpp>

#include <optional>

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

        // Puts the entity's body and Transform at a world pose without sweeping to it, whatever the motion type.
        // The rotation defaults to the current one. False when the entity has no live body or the pose does not fit.
        static bool Teleport(World &world, Entity entity, const WorldPosition &position,
            const std::optional<glm::quat> &rotation = std::nullopt);

        // PlayBegin: bodies for the RigidBodies already in the scene, then the simulation goes live
        static void OnPlayBegin(World &world);
        // PlayEnd: the simulation stops being live before the restore adds its entities
        static void OnPlayEnd(World &world);

    private:
        // New bodies and Transform edits reach Jolt before the step; Jolt's poses come back after it.
        void ApplyEdits(World &world, PhysicsWorld &physics);
        void PushTransforms(World &world, PhysicsWorld &physics, float dt);
        void PullPoses(World &world, PhysicsWorld &physics);

        Query<Transform, RigidBody> *mQuery = nullptr;
    };
}
