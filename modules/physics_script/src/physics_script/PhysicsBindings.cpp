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
#include <physics/PhysicsSystem.h>
#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>

#include <sol/sol.hpp>

#include <cctype>
#include <cmath>
#include <optional>
#include <span>
#include <string>
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

        // halfExtent becomes half_extent, the way the other Lua names are spelled.
        std::string SnakeCase(std::string_view name)
        {
            std::string out;
            for (const char c : name)
            {
                if (std::isupper(static_cast<unsigned char>(c)))
                {
                    out += '_';
                    out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                else
                    out += c;
            }
            return out;
        }

        // A RigidBody field read and written through world:get and world:set, so a script edits it the way the
        // editor does and the conversions live in one place.
        sol::object ReadRigidBodyField(sol::this_state state, World &world, Entity entity, std::string_view field)
        {
            sol::state_view lua(state);
            sol::protected_function get = lua["World"]["get"];
            const sol::protected_function_result result = get(std::ref(world), entity, "RigidBody");
            if (!result.valid())
                return sol::make_object(lua, sol::lua_nil);

            const sol::object component = result;
            if (!component.is<sol::table>())
                return sol::make_object(lua, sol::lua_nil);
            return component.as<sol::table>()[field];
        }

        bool WriteRigidBodyField(
            sol::this_state state, World &world, Entity entity, std::string_view field, const sol::object &value)
        {
            sol::state_view lua(state);
            sol::protected_function set = lua["World"]["set"];
            const sol::protected_function_result result = set(std::ref(world), entity, "RigidBody", field, value);
            return result.valid() && result.get_type() == sol::type::boolean && result.get<bool>();
        }

        // Lua tables PhysicsMotion, PhysicsShape and PhysicsAxis, filled from the same name arrays the editor shows.
        void RegisterEnum(sol::state &lua, const char *name, std::span<const std::string_view> names)
        {
            sol::table table = lua.create_table();
            for (std::size_t i = 0; i < names.size(); ++i)
                table[std::string(names[i])] = static_cast<int>(i);
            lua[name] = table;
        }

        void WorldWake(World &world, Entity entity)
        {
            if (const std::optional<BodyRef> ref = ResolveBody(world, entity))
                ref->physics.WakeBody(ref->body);
        }

        bool WorldIsSleeping(World &world, Entity entity)
        {
            const std::optional<BodyRef> ref = ResolveBody(world, entity);
            return ref && ref->physics.IsBodySleeping(ref->body);
        }

        // world:teleport(entity, position[, rotation]): a world pose, with no sweep to it on a kinematic body.
        bool WorldTeleport(World &world, Entity entity, const sol::object &position, const sol::optional<sol::object> &rotation)
        {
            const std::optional<WorldVec3> at = WorldVec3FromLua(position);
            const std::optional<WorldPosition> target = at ? WorldPosition::FromMeters(*at) : std::nullopt;
            if (!target)
            {
                MIR_LOG_ERROR("script: world:teleport needs a position table with finite x, y and z");
                return false;
            }

            std::optional<glm::quat> turned;
            if (Given(rotation))
            {
                if (!rotation->is<sol::table>())
                {
                    MIR_LOG_ERROR("script: world:teleport needs a rotation table with x, y, z and w");
                    return false;
                }
                const sol::table t = rotation->as<sol::table>();
                const glm::quat q(t.get_or("w", 1.0f), t.get_or("x", 0.0f), t.get_or("y", 0.0f), t.get_or("z", 0.0f));
                if (!std::isfinite(q.w + q.x + q.y + q.z) || glm::length(q) == 0.0f)
                {
                    MIR_LOG_ERROR("script: world:teleport needs a finite rotation");
                    return false;
                }
                turned = glm::normalize(q);
            }

            return PhysicsSystem::Teleport(world, entity, *target, turned);
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
        worldType.set_function("wake", &WorldWake);
        worldType.set_function("is_sleeping", &WorldIsSleeping);
        worldType.set_function("teleport", &WorldTeleport);

        // every RigidBody field: <name>(entity) reads it, set_<name>(entity, value) writes it
        for (const FieldDesc &field : kRigidBodyFields)
        {
            const std::string snake = SnakeCase(field.mName);
            worldType.set_function(snake,
                [name = std::string(field.mName)](sol::this_state state, World &world, Entity entity)
                { return ReadRigidBodyField(state, world, entity, name); });
            worldType.set_function("set_" + snake,
                [name = std::string(field.mName)](sol::this_state state, World &world, Entity entity, const sol::object &value)
                { return WriteRigidBodyField(state, world, entity, name, value); });
        }

        RegisterEnum(lua, "PhysicsMotion", kPhysicsMotionNames);
        RegisterEnum(lua, "PhysicsShape", kPhysicsShapeNames);
        RegisterEnum(lua, "PhysicsAxis", kPhysicsAxisNames);
        worldType.set_function("collisions", &WorldCollisions);
    }
}
