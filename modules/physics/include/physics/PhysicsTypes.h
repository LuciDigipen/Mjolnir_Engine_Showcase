/**
 * @file PhysicsTypes.h
 * @author Jackey Cen
 * @brief Handles, motion and shape types the physics module hands out.
 *
 * Deliberately free of any Jolt header: Jolt is a PRIVATE dependency of
 * engine_physics, so nothing reachable from include/physics may name it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>

namespace mir
{
    inline constexpr glm::quat kIdentityRotation{1.0f, 0.0f, 0.0f, 0.0f};

    enum class PhysicsMotion : int32_t
    {
        Static = 0,
        Dynamic = 1,
        // Moved by a script, a Transform write or a velocity, never by forces; pushes dynamic bodies and is not pushed back.
        Kinematic = 2,
    };

    // A set of motion types as a bitmask, for a ray cast that asks for only some of them.
    constexpr uint32_t MotionBit(PhysicsMotion motion) { return 1u << static_cast<uint32_t>(motion); }
    inline constexpr uint32_t kAllMotions =
        MotionBit(PhysicsMotion::Static) | MotionBit(PhysicsMotion::Dynamic) | MotionBit(PhysicsMotion::Kinematic);

    // Index == underlying PhysicsMotion value; for RigidBody's "motion" inspector field.
    inline constexpr std::string_view kPhysicsMotionNames[] = { "Static", "Dynamic", "Kinematic" };
    // Each assert names its enum's last value; move it along when one is added.
    static_assert(std::size(kPhysicsMotionNames) == static_cast<std::size_t>(PhysicsMotion::Kinematic) + 1);

    enum class PhysicsShape : int32_t
    {
        Box = 0,
        Sphere = 1,
        Capsule = 2,
    };

    // Index == underlying PhysicsShape value; for RigidBody's "shape" inspector field.
    inline constexpr std::string_view kPhysicsShapeNames[] = { "Box", "Sphere", "Capsule" };
    static_assert(std::size(kPhysicsShapeNames) == static_cast<std::size_t>(PhysicsShape::Capsule) + 1);

    // The way a capsule stands.
    enum class PhysicsAxis : int32_t
    {
        X = 0,
        Y = 1,
        Z = 2,
    };

    inline constexpr std::string_view kPhysicsAxisNames[] = { "X", "Y", "Z" };
    static_assert(std::size(kPhysicsAxisNames) == static_cast<std::size_t>(PhysicsAxis::Z) + 1);

    struct BodyHandle
    {
        static constexpr uint32_t kInvalid = 0xFFFFFFFFu;

        uint32_t mId = kInvalid;

        bool IsValid() const { return mId != kInvalid; }

        friend bool operator==(BodyHandle, BodyHandle) = default;
    };
}
