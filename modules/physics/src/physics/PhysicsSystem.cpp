/**
 * @file PhysicsSystem.cpp
 * @author Jackey Cen
 * @brief Body lifetime and the Transform <-> RigidBody sync around one step.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/PhysicsSystem.h>

#include <physics/PhysicsStepGate.h>
#include <physics/PhysicsWorld.h>

#include <core/ecs/EngineMode.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>

namespace mir
{
    namespace
    {
        // A RigidBody belongs on a root, whose Transform position is the
        // absolute world position. The setters keep it finite, so the origin
        // fallback is never taken.
        WorldPosition BodyWorldPosition(const Transform &transform)
        {
            return WorldPosition::FromMeters(transform.Position()).value_or(WorldPosition{});
        }

        void ResetStepPoses(RigidBody &rigidBody, const Transform &transform)
        {
            rigidBody.runtime.previousPosition = rigidBody.runtime.currentPosition = transform.Position();
            rigidBody.runtime.previousRotation = rigidBody.runtime.currentRotation = transform.Rotation();
        }

        // The one place a body is built: the add hook, the PlayBegin pass and the first-step fallback share it.
        void CreateBodyFor(World &world, PhysicsWorld &physics, Entity entity, const Transform &transform,
            RigidBody &rigidBody)
        {
            // Jolt is world space and Transform is parent space, so a
            // parented body would drift. Warned once, at creation.
            if (ParentOf(world, entity) != kNullEntity)
            {
                MIR_LOG_WARN("PhysicsSystem: entity {} has a parent; its RigidBody is simulated "
                             "in world space and will not follow the parent",
                    entity.mIndex);
            }

            rigidBody.runtime.body = physics.CreateBody({.mMotion = rigidBody.motion,
                                                         .mShape = rigidBody.shape,
                                                         .mPosition = BodyWorldPosition(transform),
                                                         .mRotation = transform.Rotation(),
                                                         .mHalfExtent = rigidBody.halfExtent,
                                                         .mRadius = rigidBody.radius});
            physics.SetBodyEntity(rigidBody.runtime.body, entity);
            ResetStepPoses(rigidBody, transform);
            rigidBody.runtime.transformVersion = transform.Version();
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
        PullPoses(*physics);
    }

    void PhysicsSystem::PushTransforms(World &world, PhysicsWorld &physics)
    {
        mQuery->ForEach(
            [&](Entity entity, Transform &transform, RigidBody &rigidBody)
            {
                if (!rigidBody.runtime.body.IsValid())
                {
                    // The Transform came after the RigidBody, so the add hook had nothing to build from.
                    MIR_LOG_WARN("PhysicsSystem: entity {} had no body at its first step; created it now, "
                                 "its Transform was added after its RigidBody",
                        entity.mIndex);
                    CreateBodyFor(world, physics, entity, transform, rigidBody);
                }
                else if (transform.Version() != rigidBody.runtime.transformVersion)
                {
                    MIR_ASSERT(physics.BodyEntity(rigidBody.runtime.body) == entity,
                        "PhysicsSystem: entity {} holds a body that belongs to another entity", entity.mIndex);

                    // Moved by something other than physics since the last step.
                    physics.SetBodyTransform(rigidBody.runtime.body, BodyWorldPosition(transform), transform.Rotation());
                    ResetStepPoses(rigidBody, transform);
                }

                rigidBody.runtime.transformVersion = transform.Version();
            });
    }

    void PhysicsSystem::PullPoses(PhysicsWorld &physics)
    {
        mQuery->ForEach(
            [&](Entity, Transform &transform, RigidBody &rigidBody)
            {
                if (!HasDynamicBody(rigidBody))
                    return;

                rigidBody.runtime.previousPosition = rigidBody.runtime.currentPosition;
                rigidBody.runtime.previousRotation = rigidBody.runtime.currentRotation;

                // A body Jolt left at a non-finite position keeps its last
                // Transform instead of writing that back.
                const std::optional<WorldPosition> position = physics.BodyPosition(rigidBody.runtime.body);
                if (position && position->Meters() != transform.Position())
                    transform.SetPosition(position->Meters());

                const glm::quat rotation = physics.BodyRotation(rigidBody.runtime.body);
                if (rotation != transform.Rotation())
                    transform.SetRotation(rotation);

                rigidBody.runtime.currentPosition = transform.Position();
                rigidBody.runtime.currentRotation = transform.Rotation();
                rigidBody.runtime.transformVersion = transform.Version();
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
        CreateBodyFor(world, *physics, entity, *transform, *rigidBody);
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
            [&](Entity entity, Transform &transform, RigidBody &rigidBody)
            {
                if (!rigidBody.runtime.body.IsValid())
                    CreateBodyFor(world, *physics, entity, transform, rigidBody);
            });

        physics->SetSimulationLive(true);
    }

    void PhysicsSystem::OnPlayEnd(World &world)
    {
        if (PhysicsWorld *physics = world.TryResource<PhysicsWorld>(); physics != nullptr)
            physics->SetSimulationLive(false);
    }
}
