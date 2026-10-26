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

#include <core/ecs/Query.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace mir::detail
{
    struct BodyPose
    {
        WorldPosition position;
        glm::quat rotation = kIdentityRotation;
        glm::vec3 scale{ 1.0f };
    };

    // Shear is dropped by the decomposition, which is as close as a Jolt body can get. A rotated child under a
    // non-uniform scale has shear, so the quaternion it gives is not a unit one and is made so here.
    inline BodyPose PoseOf(World &world, Entity entity)
    {
        const WorldPose pose = ResolveWorld(world, entity);
        BodyPose out;
        out.position = pose.position;
        DecomposeRotationScale(pose.linear, out.rotation, out.scale);

        const float length = glm::length(out.rotation);
        out.rotation = length > 0.0f && std::isfinite(length) ? out.rotation / length : kIdentityRotation;
        return out;
    }

    // The entities of the query whose body moves, every parent before its children. A child's local pose is worked
    // out against its parent's Transform, so the parent has to be written first or its later write moves the child's
    // world pose away from the child's body.
    inline void ParentFirst(World &world, Query<Transform, RigidBody> &query, std::vector<Entity> &out)
    {
        struct Item
        {
            uint32_t depth;
            Entity entity;
        };

        std::vector<Item> items;
        query.ForEach(
            [&](Entity entity, Transform &, RigidBody &rigidBody)
            {
                if (!HasMovingBody(rigidBody))
                    return;
                uint32_t depth = 0;
                for (Entity parent = ParentOf(world, entity); parent != kNullEntity; parent = ParentOf(world, parent))
                    ++depth;
                items.push_back({ depth, entity });
            });

        // the index breaks ties, so the order is the same every step
        std::sort(items.begin(), items.end(),
            [](const Item &a, const Item &b)
            { return a.depth != b.depth ? a.depth < b.depth : a.entity.mIndex < b.entity.mIndex; });

        out.clear();
        for (const Item &item : items)
            out.push_back(item.entity);
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
