/**
 * @file BodyPose.h
 * @author sumin.park
 * @brief An entity's world pose as physics sees it, and writing one back through the parent chain.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <physics/components/RigidBody.h>

#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <optional>

namespace mir::detail
{
    struct BodyPose
    {
        WorldPosition position;
        glm::quat rotation = kIdentityRotation;
        glm::vec3 scale{ 1.0f };
    };

    // Shear is dropped by the decomposition, which is as close as a Jolt body can get.
    inline BodyPose PoseOf(World &world, Entity entity)
    {
        const WorldPose pose = ResolveWorld(world, entity);
        BodyPose out;
        out.position = pose.position;
        DecomposeRotationScale(pose.linear, out.rotation, out.scale);
        return out;
    }

    inline void Stamp(RigidBody &rigidBody, const BodyPose &pose)
    {
        rigidBody.runtime.stampedPosition = pose.position.Meters();
        rigidBody.runtime.stampedRotation = pose.rotation;
    }

    // Something other than physics changed the pose since it was last stamped.
    inline bool PositionWritten(const RigidBody &rigidBody, const BodyPose &pose)
    {
        return pose.position.Meters() != rigidBody.runtime.stampedPosition;
    }

    inline bool RotationWritten(const RigidBody &rigidBody, const BodyPose &pose)
    {
        return pose.rotation != rigidBody.runtime.stampedRotation;
    }

    inline void ResetStepPoses(RigidBody &rigidBody, const WorldVec3 &position, const glm::quat &rotation)
    {
        rigidBody.runtime.previousPosition = rigidBody.runtime.currentPosition = position;
        rigidBody.runtime.previousRotation = rigidBody.runtime.currentRotation = rotation;
    }

    // Writes a world position and rotation to the entity's Transform through its parents, keeping its
    // world scale and its local scale. False, with the Transform untouched, when the result does not fit.
    inline bool WriteWorldPose(
        World &world, Entity entity, Transform &transform, const WorldPosition &position, const glm::quat &rotation)
    {
        glm::mat3 linear = glm::mat3_cast(rotation);
        const glm::vec3 scale = PoseOf(world, entity).scale;
        linear[0] *= scale.x;
        linear[1] *= scale.y;
        linear[2] *= scale.z;

        const Entity parent = ParentOf(world, entity);
        const WorldPose parentPose = parent == kNullEntity ? WorldPose{} : ResolveWorld(world, parent);
        const std::optional<Transform> local = TryLocalForWorldPose(parentPose, WorldPose{ position, linear });
        if (!local)
            return false;

        if (local->Position() != transform.Position() && !transform.SetPosition(local->Position()))
            return false;
        if (local->Rotation() != transform.Rotation())
            transform.SetRotation(local->Rotation());
        return true;
    }
}
