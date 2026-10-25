/**
 * @file PhysicsSystem.cpp
 * @author Jackey Cen
 * @brief Body lifetime and the Transform <-> RigidBody sync around one step.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/PhysicsSystem.h>

#include "BodyPose.h"

#include <physics/PhysicsStepGate.h>
#include <physics/PhysicsWorld.h>

#include <core/ecs/EngineMode.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>

#include <algorithm>

namespace mir
{
    namespace
    {
        using detail::BodyPose;

        // The one place a body is built: the add hook, the PlayBegin pass and the first-step fallback share it.
        // The pose is the entity's world pose, and the shape follows the world scale.
        void CreateBodyFor(World &world, PhysicsWorld &physics, Entity entity, RigidBody &rigidBody)
        {
            const BodyPose pose = detail::PoseOf(world, entity);
            const glm::vec3 scale = glm::abs(pose.scale);

            rigidBody.runtime.body = physics.CreateBody({.mMotion = rigidBody.motion,
                                                         .mShape = rigidBody.shape,
                                                         .mPosition = pose.position,
                                                         .mRotation = pose.rotation,
                                                         .mHalfExtent = rigidBody.halfExtent * scale,
                                                         .mRadius = rigidBody.radius * std::max({ scale.x, scale.y, scale.z })});
            physics.SetBodyEntity(rigidBody.runtime.body, entity);
            detail::ResetStepPoses(rigidBody, pose.position.Meters(), pose.rotation);
            detail::Stamp(rigidBody, pose);
        }

        // Spawns and teleports are already in the world, so they pass the gates like any move.
        void RunStepGates(World &world, PhysicsWorld &physics, float dt)
        {
            if (PhysicsStepGates *gates = world.TryResource<PhysicsStepGates>())
                for (PhysicsStepGate *gate : gates->mGates)
                    gate->BeforeStep(world, physics, dt);
        }
    }

    void PhysicsSystem::OnStart(SystemContext &context)
    {
        mQuery = &context.world.GetOrCreateQuery<Transform, RigidBody>();
    }

    void PhysicsSystem::OnUpdate(SystemContext &context)
    {
        if (!IsSimulating(context.world))
            return;

        PhysicsWorld *physics = context.world.TryResource<PhysicsWorld>();
        if (physics == nullptr)
            return;

        PushTransforms(context.world, *physics);
        RunStepGates(context.world, *physics, context.dt);
        physics->Step(context.dt);
        PullPoses(context.world, *physics);
    }

    void PhysicsSystem::PushTransforms(World &world, PhysicsWorld &physics)
    {
        mQuery->ForEach(
            [&](Entity entity, Transform &, RigidBody &rigidBody)
            {
                if (!rigidBody.runtime.body.IsValid())
                {
                    // The Transform came after the RigidBody, so the add hook had nothing to build from.
                    MIR_LOG_WARN("PhysicsSystem: entity {} had no body at its first step; created it now, "
                                 "its Transform was added after its RigidBody",
                        entity.mIndex);
                    CreateBodyFor(world, physics, entity, rigidBody);
                    return;
                }

                MIR_ASSERT(physics.BodyEntity(rigidBody.runtime.body) == entity,
                    "PhysicsSystem: entity {} holds a body that belongs to another entity", entity.mIndex);

                // Moved by something other than physics since the last step, its own Transform or a parent's.
                const BodyPose pose = detail::PoseOf(world, entity);
                const bool positionWritten = detail::PositionWritten(rigidBody, pose);
                const bool rotationWritten = detail::RotationWritten(rigidBody, pose);
                if (!positionWritten && !rotationWritten)
                    return;

                // The entity may show a lagging interpolated pose, so what nobody wrote comes from the body.
                WorldPosition position = pose.position;
                if (!positionWritten)
                    position = physics.BodyPosition(rigidBody.runtime.body).value_or(pose.position);
                const glm::quat rotation =
                    rotationWritten ? pose.rotation : physics.BodyRotation(rigidBody.runtime.body);

                physics.SetBodyTransform(rigidBody.runtime.body, position, rotation);
                // interpolation restarts from here, with no blend from the old pose
                detail::ResetStepPoses(rigidBody, position.Meters(), rotation);
                // stamp what the entity shows, not the target, or the next push would see a write again
                detail::Stamp(rigidBody, pose);
            });
    }

    void PhysicsSystem::PullPoses(World &world, PhysicsWorld &physics)
    {
        mQuery->ForEach(
            [&](Entity entity, Transform &transform, RigidBody &rigidBody)
            {
                if (!HasDynamicBody(rigidBody))
                    return;

                rigidBody.runtime.previousPosition = rigidBody.runtime.currentPosition;
                rigidBody.runtime.previousRotation = rigidBody.runtime.currentRotation;

                // A body Jolt left at a non-finite position, or one that does not fit back through its
                // parents, keeps its last Transform instead of writing that back.
                if (const std::optional<WorldPosition> position = physics.BodyPosition(rigidBody.runtime.body))
                    detail::WriteWorldPose(world, entity, transform, *position, physics.BodyRotation(rigidBody.runtime.body));

                // What the entity resolves to now, not Jolt's pose: the round trip through the parents can
                // differ by float rounding, and the next push compares against this.
                const BodyPose pose = detail::PoseOf(world, entity);
                rigidBody.runtime.currentPosition = pose.position.Meters();
                rigidBody.runtime.currentRotation = pose.rotation;
                detail::Stamp(rigidBody, pose);
            });
    }

    void PhysicsSystem::OnRigidBodyAdded(World &world, Entity entity)
    {
        RigidBody *rigidBody = world.GetComponent<RigidBody>(entity);
        if (rigidBody == nullptr)
            return;

        // First, before any early return: a copy must not keep the original's handle.
        rigidBody->runtime = {};

        PhysicsWorld *physics = world.TryResource<PhysicsWorld>();
        Transform *transform = world.GetComponent<Transform>(entity);
        if (physics == nullptr || !physics->IsSimulationLive() || transform == nullptr)
            return;

        MIR_ASSERT(!physics->IsStepping(), "PhysicsSystem: a RigidBody was added while physics was stepping");
        CreateBodyFor(world, *physics, entity, *rigidBody);
    }

    void PhysicsSystem::OnRigidBodyRemoved(World &world, Entity entity)
    {
        RigidBody *rigidBody = world.GetComponent<RigidBody>(entity);
        if (rigidBody == nullptr || !rigidBody->runtime.body.IsValid())
            return;

        if (PhysicsWorld *physics = world.TryResource<PhysicsWorld>(); physics != nullptr)
        {
            MIR_ASSERT(physics->BodyEntity(rigidBody->runtime.body) == entity,
                "PhysicsSystem: entity {} frees a body that belongs to another entity", entity.mIndex);
            physics->DestroyBody(rigidBody->runtime.body);
        }

        rigidBody->runtime.body = {};
    }

    void PhysicsSystem::OnPlayBegin(World &world)
    {
        PhysicsWorld *physics = world.TryResource<PhysicsWorld>();
        if (physics == nullptr)
            return;

        MIR_ASSERT(!physics->IsSimulationLive(), "PhysicsSystem: PlayBegin ran twice without a PlayEnd");

        world.ForEach<Transform, RigidBody>(
            [&](Entity entity, Transform &, RigidBody &rigidBody)
            {
                if (!rigidBody.runtime.body.IsValid())
                    CreateBodyFor(world, *physics, entity, rigidBody);
            });

        physics->SetSimulationLive(true);
    }

    void PhysicsSystem::OnPlayEnd(World &world)
    {
        if (PhysicsWorld *physics = world.TryResource<PhysicsWorld>(); physics != nullptr)
            physics->SetSimulationLive(false);
    }
}
