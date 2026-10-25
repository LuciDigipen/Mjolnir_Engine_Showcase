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

        BodyHandle CreateBodyFor(PhysicsWorld &physics, const RigidBody &rigidBody, const Transform &transform)
        {
            return physics.CreateBody({.mMotion = rigidBody.motion,
                                       .mShape = rigidBody.shape,
                                       .mPosition = BodyWorldPosition(transform),
                                       .mRotation = transform.Rotation(),
                                       .mHalfExtent = rigidBody.halfExtent,
                                       .mRadius = rigidBody.radius});
        }

        void ResetStepPoses(RigidBody &rigidBody, const Transform &transform)
        {
            rigidBody.previousPosition = rigidBody.currentPosition = transform.Position();
            rigidBody.previousRotation = rigidBody.currentRotation = transform.Rotation();
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
                if (!rigidBody.body.IsValid())
                {
                    // Jolt is world space and Transform is parent space, so a
                    // parented body would drift. Warned once, at creation.
                    if (ParentOf(world, entity) != kNullEntity)
                    {
                        MIR_LOG_WARN("PhysicsSystem: entity {} has a parent; its RigidBody is simulated "
                                     "in world space and will not follow the parent",
                            entity.mIndex);
                    }

                    // This runs after ScriptSystem PreUpdate phase,
                    // meaning OnStart() in a script can never find a body.
                    rigidBody.body = CreateBodyFor(physics, rigidBody, transform);
                    physics.SetBodyEntity(rigidBody.body, entity);
                    ResetStepPoses(rigidBody, transform);
                }
                else if (transform.Version() != rigidBody.transformVersion)
                {
                    // Moved by something other than physics since the last step.
                    physics.SetBodyTransform(rigidBody.body, BodyWorldPosition(transform), transform.Rotation());
                    ResetStepPoses(rigidBody, transform);
                }

                rigidBody.transformVersion = transform.Version();
            });
    }

    void PhysicsSystem::PullPoses(PhysicsWorld &physics)
    {
        mQuery->ForEach(
            [&](Entity, Transform &transform, RigidBody &rigidBody)
            {
                if (!HasDynamicBody(rigidBody))
                    return;

                rigidBody.previousPosition = rigidBody.currentPosition;
                rigidBody.previousRotation = rigidBody.currentRotation;

                // A body Jolt left at a non-finite position keeps its last
                // Transform instead of writing that back.
                const std::optional<WorldPosition> position = physics.BodyPosition(rigidBody.body);
                if (position && position->Meters() != transform.Position())
                    transform.SetPosition(position->Meters());

                const glm::quat rotation = physics.BodyRotation(rigidBody.body);
                if (rotation != transform.Rotation())
                    transform.SetRotation(rotation);

                rigidBody.currentPosition = transform.Position();
                rigidBody.currentRotation = transform.Rotation();
                rigidBody.transformVersion = transform.Version();
            });
    }

    void PhysicsSystem::OnRigidBodyRemoved(World &world, Entity entity)
    {
        RigidBody *rigidBody = world.GetComponent<RigidBody>(entity);
        if (rigidBody == nullptr || !rigidBody->body.IsValid())
            return;

        if (PhysicsWorld *physics = world.TryResource<PhysicsWorld>(); physics != nullptr)
            physics->DestroyBody(rigidBody->body);

        rigidBody->body = {};
    }
}
