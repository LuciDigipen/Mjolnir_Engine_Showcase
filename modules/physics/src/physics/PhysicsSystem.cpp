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

        // The shape follows the world scale: a box per axis, a sphere by the largest axis, a capsule by the
        // axis it stands along for its height and the larger of the other two for its radius. The collider
        // offset scales with it; a rotated non-uniform parent is as close as the decomposition gets.
        // A layer outside 0 to 31 is the Default layer, said once here at creation and at the edit.
        int LayerOf(const RigidBody &rigidBody, Entity entity)
        {
            if (rigidBody.layer >= 0 && rigidBody.layer < kPhysicsLayerCount)
                return rigidBody.layer;

            MIR_LOG_WARN("PhysicsSystem: entity {} has layer {}, which is outside 0 to {}; using layer 0",
                entity.mIndex, rigidBody.layer, kPhysicsLayerCount - 1);
            return 0;
        }

        BodyDesc BodyDescFor(const RigidBody &rigidBody, const BodyPose &pose, Entity entity)
        {
            const glm::vec3 scale = glm::abs(pose.scale);
            const int axis = static_cast<int>(rigidBody.axis);
            const float along = scale[axis];
            const float across = std::max(scale[(axis + 1) % 3], scale[(axis + 2) % 3]);
            const bool capsule = rigidBody.shape == PhysicsShape::Capsule;

            return {.mMotion = rigidBody.motion,
                    .mShape = rigidBody.shape,
                    .mPosition = pose.position,
                    .mRotation = pose.rotation,
                    .mHalfExtent = rigidBody.halfExtent * scale,
                    .mRadius = rigidBody.radius * (capsule ? across : std::max({ scale.x, scale.y, scale.z })),
                    .mHalfHeight = rigidBody.halfHeight * along,
                    .mAxis = rigidBody.axis,
                    .mOffset = rigidBody.offset * pose.scale,
                    .mRotationOffset = rigidBody.rotationOffset,
                    .mLayer = LayerOf(rigidBody, entity),
                    .mIsTrigger = rigidBody.isTrigger,
                    .mDetectStay = rigidBody.detectStay,
                    .mMass = rigidBody.mass,
                    .mGravityFactor = rigidBody.gravityFactor,
                    .mFriction = rigidBody.friction,
                    .mRestitution = rigidBody.restitution,
                    .mAllowSleeping = rigidBody.allowSleeping};
        }

        // The one place a body is built: the add hook, the PlayBegin pass and the first-step fallback share it.
        // The pose is the entity's world pose, and the shape follows the world scale.
        // A body the physics world refuses (a size at or below 0) is left out, on purpose: the reason was logged
        // by that first try, and `quiet` keeps the retry after an edit from saying it again.
        void CreateBodyFor(World &world, PhysicsWorld &physics, Entity entity, RigidBody &rigidBody, bool quiet = false)
        {
            const BodyPose pose = detail::PoseOf(world, entity);

            BodyDesc desc = BodyDescFor(rigidBody, pose, entity);
            desc.mQuiet = quiet;
            rigidBody.runtime.body = physics.CreateBody(desc);
            rigidBody.runtime.applied = AppliedOf(rigidBody);
            rigidBody.runtime.creationRefused = !rigidBody.runtime.body.IsValid();
            if (rigidBody.runtime.creationRefused)
                return;

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

        ApplyEdits(context.world, *physics);
        PushTransforms(context.world, *physics, context.dt);
        RunStepGates(context.world, *physics, context.dt);
        physics->Step(context.dt);
        PullPoses(context.world, *physics);
    }

    // An edit to a RigidBody, from the editor, Lua or code, reaches the live body here, before the step.
    void PhysicsSystem::ApplyEdits(World &world, PhysicsWorld &physics)
    {
        mQuery->ForEach(
            [&](Entity entity, Transform &, RigidBody &rigidBody)
            {
                if (!rigidBody.runtime.body.IsValid())
                {
                    // refused at creation: an edit may have made it buildable, and says nothing if it still is not
                    if (rigidBody.runtime.creationRefused && AppliedOf(rigidBody) != rigidBody.runtime.applied)
                        CreateBodyFor(world, physics, entity, rigidBody, true);
                    return;
                }

                const AppliedBody now = AppliedOf(rigidBody);
                AppliedBody &applied = rigidBody.runtime.applied;
                const BodyHandle body = rigidBody.runtime.body;
                // a held body is left alone, and the edit lands once it is resumed
                if (now == applied || physics.IsBodyHeld(body))
                    return;

                if (now.motion != applied.motion)
                    physics.SetBodyMotion(body, now.motion);

                if (now.shape != applied.shape || now.halfExtent != applied.halfExtent || now.radius != applied.radius
                    || now.halfHeight != applied.halfHeight || now.axis != applied.axis || now.offset != applied.offset
                    || now.rotationOffset != applied.rotationOffset)
                    physics.SetBodyShape(body, BodyDescFor(rigidBody, detail::PoseOf(world, entity), entity));
                else if (now.mass != applied.mass)
                    physics.SetBodyMass(body, now.mass);

                if (now.layer != applied.layer)
                    physics.SetBodyLayer(body, LayerOf(rigidBody, entity));
                if (now.isTrigger != applied.isTrigger)
                    physics.SetBodyTrigger(body, now.isTrigger);
                if (now.friction != applied.friction || now.restitution != applied.restitution)
                    physics.SetBodyMaterial(body, now.friction, now.restitution);
                if (now.gravityFactor != applied.gravityFactor)
                    physics.SetBodyGravityFactor(body, now.gravityFactor);
                if (now.allowSleeping != applied.allowSleeping)
                    physics.SetBodyAllowSleeping(body, now.allowSleeping);
                if (now.detectStay != applied.detectStay)
                    physics.SetBodyDetectsStay(body, now.detectStay);

                applied = now;
            });
    }

    void PhysicsSystem::PushTransforms(World &world, PhysicsWorld &physics, float dt)
    {
        mQuery->ForEach(
            [&](Entity entity, Transform &, RigidBody &rigidBody)
            {
                // refused already: it is tried again when its fields change, in ApplyEdits
                if (!rigidBody.runtime.body.IsValid() && rigidBody.runtime.creationRefused)
                    return;

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

                if (rigidBody.motion == PhysicsMotion::Kinematic && rigidBody.runtime.hasMoved)
                {
                    // moved to the target over this step, so dynamic bodies in the way are pushed; the step
                    // poses keep running, so interpolation draws the sweep
                    physics.MoveBodyKinematic(rigidBody.runtime.body, position, rotation, dt);
                    rigidBody.runtime.movedToTarget = true;
                }
                else
                {
                    physics.SetBodyTransform(rigidBody.runtime.body, position, rotation);
                    // interpolation restarts from here, with no blend from the old pose
                    detail::ResetStepPoses(rigidBody, position.Meters(), rotation);
                    rigidBody.runtime.hasMoved = true;
                }
                // stamp what the entity shows, not the target, or the next push would see a write again
                detail::Stamp(rigidBody, pose);
            });
    }

    void PhysicsSystem::PullPoses(World &world, PhysicsWorld &physics)
    {
        mQuery->ForEach(
            [&](Entity entity, Transform &transform, RigidBody &rigidBody)
            {
                if (!HasMovingBody(rigidBody))
                    return;

                // The step reached the target, so the velocity MoveKinematic set must not carry it on.
                if (rigidBody.runtime.movedToTarget)
                {
                    physics.SetBodyLinearVelocity(rigidBody.runtime.body, glm::vec3(0.0f));
                    physics.SetBodyAngularVelocity(rigidBody.runtime.body, glm::vec3(0.0f));
                    rigidBody.runtime.movedToTarget = false;
                }

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

    bool PhysicsSystem::Teleport(World &world, Entity entity, const WorldPosition &position,
        const std::optional<glm::quat> &rotation)
    {
        PhysicsWorld *physics = world.TryResource<PhysicsWorld>();
        RigidBody *rigidBody = world.GetComponent<RigidBody>(entity);
        Transform *transform = world.GetComponent<Transform>(entity);
        if (physics == nullptr || rigidBody == nullptr || transform == nullptr || !rigidBody->runtime.body.IsValid())
            return false;

        const glm::quat target = rotation.value_or(detail::PoseOf(world, entity).rotation);
        if (!detail::WriteWorldPose(world, entity, *transform, position, target))
            return false;

        // from what the entity resolves to, so the next push finds nothing written
        const BodyPose pose = detail::PoseOf(world, entity);
        physics->SetBodyTransform(rigidBody->runtime.body, pose.position, pose.rotation);
        detail::ResetStepPoses(*rigidBody, pose.position.Meters(), pose.rotation);
        detail::Stamp(*rigidBody, pose);
        rigidBody->runtime.hasMoved = true;
        return true;
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
                if (!rigidBody.runtime.body.IsValid() && !rigidBody.runtime.creationRefused)
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
