/**
 * @file JoltConversions.h
 * @author sumin.park
 * @brief glm, WorldPosition and Entity to and from their Jolt forms.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <physics/PhysicsTypes.h>

#include <core/ecs/Entity.h>
#include <core/math/WorldCoordinates.h>

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Body/BodyID.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>

namespace mir::detail
{
    inline JPH::Vec3 ToJolt(const glm::vec3 &v)
    {
        return JPH::Vec3(v.x, v.y, v.z);
    }

    inline JPH::Quat ToJolt(const glm::quat &q)
    {
        return JPH::Quat(q.x, q.y, q.z, q.w);
    }

    inline glm::quat FromJolt(JPH::QuatArg q)
    {
        return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
    }

    inline glm::vec3 FromJolt(JPH::Vec3Arg v)
    {
        return glm::vec3(v.GetX(), v.GetY(), v.GetZ());
    }

    // A Jolt of another precision would round or widen every world position on
    // the way in. CMake and JoltRuntime check the package; this checks what this
    // file compiled against.
    static_assert(sizeof(JPH::Real) == sizeof(WorldReal),
                  "JPH_DOUBLE_PRECISION must match MIR_LARGE_WORLD (the large-world vcpkg feature)");

    inline JPH::RVec3 ToJolt(WorldPosition position)
    {
        const WorldVec3 meters = position.Meters();
        return JPH::RVec3(meters.x, meters.y, meters.z);
    }

    inline std::optional<WorldPosition> PositionFromJolt(JPH::RVec3Arg v)
    {
        return WorldPosition::FromMeters(WorldVec3(v.GetX(), v.GetY(), v.GetZ()));
    }

    inline JPH::BodyID ToBodyId(BodyHandle handle)
    {
        return JPH::BodyID(handle.mId);
    }

    // Complemented so Jolt's default user data reads as no entity, not Entity{0, 0}.
    inline uint64_t EntityToUserData(Entity entity)
    {
        return entity.IsNull() ? 0 : ~PackEntity(entity);
    }

    inline Entity EntityFromUserData(uint64_t data)
    {
        const Entity entity = UnpackEntity(~data);
        return entity.IsNull() ? kNullEntity : entity;
    }
}
