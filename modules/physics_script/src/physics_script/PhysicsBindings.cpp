/**
 * @file PhysicsBindings.cpp
 * @author Jackey Cen
 * @brief Extends the World usertype with RigidBody velocity, force and impulse calls and world:raycast.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <physics_script/PhysicsBindings.h>

#include <script/BindingHelpers.h>

#include <core/ecs/World.h>
#include <core/log/Log.h>
#include <physics/CollisionEvents.h>
#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>

#include <sol/sol.hpp>

#include <optional>
#include <string_view>

namespace mir
{
    namespace
    {
        constexpr float kRayCastDefaultMaxDistance = 1000.0f;

        bool Given(const sol::optional<sol::object> &argument)
        {
            return argument.has_value() && argument->valid();
        }

        struct BodyRef
        {
            PhysicsWorld &physics;
            BodyHandle body;
        };

        // Looked up on every Lua call, so nothing caches a body.
        std::optional<BodyRef> ResolveBody(World &world, Entity entity)
        {
            PhysicsWorld *physics = world.TryResource<PhysicsWorld>();
            if (physics == nullptr)
                return std::nullopt;

            const RigidBody *rigidBody = world.GetComponent<RigidBody>(entity);
            if (rigidBody == nullptr || !rigidBody->runtime.body.IsValid())
                return std::nullopt;

            return BodyRef{*physics, rigidBody->runtime.body};
        }

        using BodyRead = glm::vec3 (PhysicsWorld::*)(BodyHandle) const;
        using BodyWrite = void (PhysicsWorld::*)(BodyHandle, const glm::vec3 &);
        using BodyWriteAtOffset = void (PhysicsWorld::*)(BodyHandle, const glm::vec3 &, const glm::vec3 &);

        sol::table ReadVector(World &world, Entity entity, sol::this_state state, BodyRead read)
        {
            const std::optional<BodyRef> ref = ResolveBody(world, entity);

            return Vec3ToLua(sol::state_view(state), ref ? (ref->physics.*read)(ref->body) : glm::vec3(0.0f));
        }

        bool WriteVector(World &world, Entity entity, std::string_view name, const sol::object &value,
                         BodyWrite write)
        {
            glm::vec3 vector{};
            if (!ToVec3(value, vector))
            {
                MIR_LOG_ERROR("script: world:{} needs a table with x, y and z", name);
                return false;
            }

            const std::optional<BodyRef> ref = ResolveBody(world, entity);
            if (!ref)
                return false;

            (ref->physics.*write)(ref->body, vector);
            return true;
        }

        bool WriteVectorAtOffset(World &world, Entity entity, std::string_view name, const sol::object &value,
                                 const sol::optional<sol::object> &offset, BodyWrite write,
                                 BodyWriteAtOffset writeAtOffset)
        {
            if (!Given(offset))
                return WriteVector(world, entity, name, value, write);

            glm::vec3 vector{};
            glm::vec3 localOffset{};
            if (!ToVec3(value, vector) || !ToVec3(*offset, localOffset))
            {
                MIR_LOG_ERROR("script: world:{} needs tables with x, y and z", name);
                return false;
            }

            const std::optional<BodyRef> ref = ResolveBody(world, entity);
            if (!ref)
                return false;

            (ref->physics.*writeAtOffset)(ref->body, vector, localOffset);
            return true;
        }

        sol::table WorldLinearVelocity(World &world, Entity entity, sol::this_state state)
        {
            return ReadVector(world, entity, state, &PhysicsWorld::BodyLinearVelocity);
        }

        sol::table WorldAngularVelocity(World &world, Entity entity, sol::this_state state)
        {
            return ReadVector(world, entity, state, &PhysicsWorld::BodyAngularVelocity);
        }

        bool WorldSetLinearVelocity(World &world, Entity entity, const sol::object &velocity)
        {
            return WriteVector(world, entity, "set_linear_velocity", velocity,
                               &PhysicsWorld::SetBodyLinearVelocity);
        }

        bool WorldSetAngularVelocity(World &world, Entity entity, const sol::object &velocity)
        {
            return WriteVector(world, entity, "set_angular_velocity", velocity,
                               &PhysicsWorld::SetBodyAngularVelocity);
        }

        bool WorldAddLinearVelocity(World &world, Entity entity, const sol::object &velocity)
        {
            return WriteVector(world, entity, "add_linear_velocity", velocity,
                               &PhysicsWorld::AddLinearVelocity);
        }

        bool WorldAddAngularVelocity(World &world, Entity entity, const sol::object &velocity)
        {
            return WriteVector(world, entity, "add_angular_velocity", velocity,
                               &PhysicsWorld::AddAngularVelocity);
        }

        // The force is world space; only the offset is in body space. A script
        // that wants a body-local thrust rotates it by Transform.rotation first.
        bool WorldAddForce(World &world, Entity entity, const sol::object &force, sol::optional<sol::object> offset)
        {
            return WriteVectorAtOffset(world, entity, "add_force", force, offset, &PhysicsWorld::AddForce,
                                       &PhysicsWorld::AddForceAtOffset);
        }

        bool WorldAddTorque(World &world, Entity entity, const sol::object &torque)
        {
            return WriteVector(world, entity, "add_torque", torque, &PhysicsWorld::AddTorque);
        }

        bool WorldAddImpulse(World &world, Entity entity, const sol::object &impulse,
                             sol::optional<sol::object> offset)
        {
            return WriteVectorAtOffset(world, entity, "add_impulse", impulse, offset, &PhysicsWorld::AddImpulse,
                                       &PhysicsWorld::AddImpulseAtOffset);
        }

        bool WorldAddAngularImpulse(World &world, Entity entity, const sol::object &impulse)
        {
            return WriteVector(world, entity, "add_angular_impulse", impulse, &PhysicsWorld::AddAngularImpulse);
        }

        // Only converts; PhysicsWorld::RayCast refuses, and logs, a bad direction or distance.
        sol::table WorldRayCast(World &world, const sol::object &origin, const sol::object &direction,
                                sol::optional<sol::object> maxDistance, sol::this_state state)
        {
            sol::state_view lua(state);
            sol::table result = lua.create_table();
            result["hit"] = false;

            const std::optional<WorldVec3> from = WorldVec3FromLua(origin);
            if (!from)
            {
                MIR_LOG_ERROR("script: world:raycast needs an origin table with finite x, y and z in world range");
                return result;
            }

            glm::vec3 dir{};
            if (!ToVec3(direction, dir))
            {
                MIR_LOG_ERROR("script: world:raycast needs a direction table with x, y and z");
                return result;
            }

            float distance = kRayCastDefaultMaxDistance;
            if (Given(maxDistance))
            {
                if (maxDistance->get_type() != sol::type::number)
                {
                    MIR_LOG_ERROR("script: world:raycast needs a number for maxDistance");
                    return result;
                }
                distance = static_cast<float>(maxDistance->as<double>());
            }

            const PhysicsWorld *physics = world.TryResource<PhysicsWorld>();
            if (physics == nullptr)
                return result;

            const std::optional<WorldPosition> position = WorldPosition::FromMeters(*from);
            if (!position)
                return result;

            const std::optional<RayHit> hit = physics->RayCast(*position, dir, distance);
            if (!hit)
                return result;

            result["hit"] = true;
            result["point"] = WorldVec3ToLua(lua, hit->mPoint.Meters());
            result["normal"] = Vec3ToLua(lua, hit->mNormal);
            result["distance"] = hit->mDistance;
            const Entity entity = physics->BodyEntity(hit->mBody);
            if (!entity.IsNull() && world.IsAlive(entity))
                result["entity"] = entity;
            return result;
        }

        // From entity's side: normal points from other toward it; other is nil when untagged or dead.
        sol::table WorldCollisions(World &world, Entity entity, sol::this_state state)
        {
            sol::state_view lua(state);
            sol::table out = lua.create_table();

            const CollisionEvents *events = world.TryResource<CollisionEvents>();
            if (events == nullptr || !world.IsAlive(entity))
                return out;

            int count = 0;
            for (const CollisionEvent &event : events->mEvents)
            {
                const bool isA = event.mEntityA == entity;
                if (!isA && event.mEntityB != entity)
                    continue;

                const Entity other = isA ? event.mEntityB : event.mEntityA;
                sol::table collision = lua.create_table();
                if (world.IsAlive(other))
                    collision["other"] = other;
                collision["point"] = WorldVec3ToLua(lua, event.mPoint.Meters());
                collision["normal"] = Vec3ToLua(lua, isA ? -event.mNormal : event.mNormal);
                collision["phase"] = event.mPhase == CollisionPhase::Begin ? "begin"
                    : event.mPhase == CollisionPhase::End                  ? "end"
                                                                           : "stay";
                out[++count] = collision;
            }
            return out;
        }
    }

    void RegisterPhysicsBindings(sol::state &lua)
    {
        sol::table worldType = lua["World"];
        worldType.set_function("linear_velocity", &WorldLinearVelocity);
        worldType.set_function("angular_velocity", &WorldAngularVelocity);
        worldType.set_function("set_linear_velocity", &WorldSetLinearVelocity);
        worldType.set_function("set_angular_velocity", &WorldSetAngularVelocity);
        worldType.set_function("add_linear_velocity", &WorldAddLinearVelocity);
        worldType.set_function("add_angular_velocity", &WorldAddAngularVelocity);
        worldType.set_function("add_force", &WorldAddForce);
        worldType.set_function("add_torque", &WorldAddTorque);
        worldType.set_function("add_impulse", &WorldAddImpulse);
        worldType.set_function("add_angular_impulse", &WorldAddAngularImpulse);
        worldType.set_function("raycast", &WorldRayCast);
        worldType.set_function("collisions", &WorldCollisions);
    }
}
