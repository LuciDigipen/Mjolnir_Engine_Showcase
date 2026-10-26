/**
 * @file PhysicsBindings.cpp
 * @author Jackey Cen
 * @brief Extends the World usertype with RigidBody velocity, force and impulse calls and world:raycast.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <physics_script/PhysicsBindings.h>

#include <script/BindingApi.h>
#include <script/BindingHelpers.h>

#include <core/ecs/World.h>
#include <core/log/Log.h>
#include <physics/CollisionEvents.h>
#include <physics_layers/PhysicsLayers.h>
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
#include <vector>

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
        // These say whether the body took the call; PhysicsWorld logs why when it did not.
        using BodyWrite = bool (PhysicsWorld::*)(BodyHandle, const glm::vec3 &);
        using BodyWriteAtOffset = bool (PhysicsWorld::*)(BodyHandle, const glm::vec3 &, const glm::vec3 &);

        sol::table ReadVector(World &world, Entity entity, sol::this_state state, BodyRead read)
        {
            const std::optional<BodyRef> ref = ResolveBody(world, entity);

            return Vec3ToLua(sol::state_view(state), ref ? (ref->physics.*read)(ref->body) : glm::vec3(0.0f));
        }

        bool WriteVector(World &world, Entity entity, std::string_view name, std::string_view argument,
                         const sol::object &value, BodyWrite write)
        {
            glm::vec3 vector{};
            if (!ReadVec3(value, vector, "world:" + std::string(name), argument))
                return false;

            const std::optional<BodyRef> ref = ResolveBody(world, entity);
            if (!ref)
                return false;

            return (ref->physics.*write)(ref->body, vector);
        }

        bool WriteVectorAtOffset(World &world, Entity entity, std::string_view name, std::string_view argument,
                                 const sol::object &value, const sol::optional<sol::object> &offset, BodyWrite write,
                                 BodyWriteAtOffset writeAtOffset)
        {
            if (!Given(offset))
                return WriteVector(world, entity, name, argument, value, write);

            const std::string call = "world:" + std::string(name);
            glm::vec3 vector{};
            glm::vec3 localOffset{};
            if (!ReadVec3(value, vector, call, argument) || !ReadVec3(*offset, localOffset, call, "offset"))
                return false;

            const std::optional<BodyRef> ref = ResolveBody(world, entity);
            if (!ref)
                return false;

            return (ref->physics.*writeAtOffset)(ref->body, vector, localOffset);
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
            return WriteVector(world, entity, "set_linear_velocity", "velocity", velocity,
                               &PhysicsWorld::SetBodyLinearVelocity);
        }

        bool WorldSetAngularVelocity(World &world, Entity entity, const sol::object &velocity)
        {
            return WriteVector(world, entity, "set_angular_velocity", "velocity", velocity,
                               &PhysicsWorld::SetBodyAngularVelocity);
        }

        bool WorldAddLinearVelocity(World &world, Entity entity, const sol::object &velocity)
        {
            return WriteVector(world, entity, "add_linear_velocity", "velocity", velocity,
                               &PhysicsWorld::AddLinearVelocity);
        }

        bool WorldAddAngularVelocity(World &world, Entity entity, const sol::object &velocity)
        {
            return WriteVector(world, entity, "add_angular_velocity", "velocity", velocity,
                               &PhysicsWorld::AddAngularVelocity);
        }

        // The force is world space; only the offset is in body space. A script
        // that wants a body-local thrust rotates it by Transform.rotation first.
        bool WorldAddForce(World &world, Entity entity, const sol::object &force, sol::optional<sol::object> offset)
        {
            return WriteVectorAtOffset(world, entity, "add_force", "force", force, offset, &PhysicsWorld::AddForce,
                                       &PhysicsWorld::AddForceAtOffset);
        }

        bool WorldAddTorque(World &world, Entity entity, const sol::object &torque)
        {
            return WriteVector(world, entity, "add_torque", "torque", torque, &PhysicsWorld::AddTorque);
        }

        bool WorldAddImpulse(World &world, Entity entity, const sol::object &impulse,
                             sol::optional<sol::object> offset)
        {
            return WriteVectorAtOffset(world, entity, "add_impulse", "impulse", impulse, offset, &PhysicsWorld::AddImpulse,
                                       &PhysicsWorld::AddImpulseAtOffset);
        }

        bool WorldAddAngularImpulse(World &world, Entity entity, const sol::object &impulse)
        {
            return WriteVector(world, entity, "add_angular_impulse", "impulse", impulse, &PhysicsWorld::AddAngularImpulse);
        }

        // Only converts; PhysicsWorld::RayCast refuses, and logs, a bad direction or distance.
        const PhysicsLayerTable &LayerTableOf(World &world);
        std::optional<int> ResolveLayer(const PhysicsLayerTable &table, const sol::object &value, std::string_view call);

        // The caster does not hit itself unless it says so.
        void ExcludeCaster(World &world, sol::state_view lua, std::vector<BodyHandle> &excluded)
        {
            const Entity caster = RunningScriptEntity(lua);
            if (caster.IsNull())
                return;
            if (const std::optional<BodyRef> ref = ResolveBody(world, caster))
                excluded.push_back(ref->body);
        }

        // The options table of world:raycast, turned into the filter and the bodies it excludes.
        // False, logged, for anything it does not understand, so a typo never quietly casts without its filter.
        bool ReadCastOptions(World &world, sol::state_view lua, const sol::table &options, RayFilter &filter,
            std::vector<BodyHandle> &excluded)
        {
            bool ignoreSelf = true;

            for (const auto &[key, value] : options)
            {
                const std::string name = key.is<std::string>() ? key.as<std::string>() : std::string();
                if (name == "layers")
                {
                    if (!value.is<sol::table>())
                    {
                        MIR_LOG_ERROR("script: world:raycast options.layers needs a list of layer names or indices");
                        return false;
                    }
                    const PhysicsLayerTable &table = LayerTableOf(world);
                    filter.mLayers = 0;
                    for (const auto &[position, layer] : value.as<sol::table>())
                    {
                        const std::optional<int> index = ResolveLayer(table, layer, "world:raycast options.layers");
                        if (!index)
                            return false;
                        filter.mLayers |= 1u << *index;
                    }
                }
                else if (name == "motion")
                {
                    if (!value.is<sol::table>())
                    {
                        MIR_LOG_ERROR("script: world:raycast options.motion needs a list of PhysicsMotion values");
                        return false;
                    }
                    filter.mMotions = 0;
                    for (const auto &[position, motion] : value.as<sol::table>())
                    {
                        if (motion.get_type() != sol::type::number)
                        {
                            MIR_LOG_ERROR("script: world:raycast options.motion needs PhysicsMotion values");
                            return false;
                        }
                        const double number = motion.as<double>();
                        if (std::floor(number) != number || number < 0.0 || number > 2.0)
                        {
                            MIR_LOG_ERROR("script: world:raycast options.motion has {}, which is not a PhysicsMotion", number);
                            return false;
                        }
                        filter.mMotions |= 1u << static_cast<int>(number);
                    }
                }
                else if (name == "exclude")
                {
                    if (!value.is<sol::table>())
                    {
                        MIR_LOG_ERROR("script: world:raycast options.exclude needs a list of entities");
                        return false;
                    }
                    for (const auto &[position, entry] : value.as<sol::table>())
                    {
                        if (!entry.is<Entity>())
                        {
                            MIR_LOG_ERROR("script: world:raycast options.exclude needs a list of entities");
                            return false;
                        }
                        if (const std::optional<BodyRef> ref = ResolveBody(world, entry.as<Entity>()))
                            excluded.push_back(ref->body);
                    }
                }
                else if (name == "ignoreSelf" || name == "hitTriggers")
                {
                    if (value.get_type() != sol::type::boolean)
                    {
                        MIR_LOG_ERROR("script: world:raycast options.{} needs true or false", name);
                        return false;
                    }
                    if (name == "ignoreSelf")
                        ignoreSelf = value.as<bool>();
                    else
                        filter.mHitTriggers = value.as<bool>();
                }
                else
                {
                    MIR_LOG_ERROR("script: world:raycast has no option '{}'; the options are layers, motion, exclude, "
                                  "ignoreSelf and hitTriggers", name);
                    return false;
                }
            }

            if (ignoreSelf)
                ExcludeCaster(world, lua, excluded);
            filter.mExclude = excluded;
            return true;
        }

        sol::table WorldRayCast(World &world, const sol::object &origin, const sol::object &direction,
                                sol::optional<sol::object> maxDistance, sol::optional<sol::object> options,
                                sol::this_state state)
        {
            sol::state_view lua(state);
            sol::table result = lua.create_table();
            result["hit"] = false;

            const std::optional<WorldVec3> from = ReadWorldVec3(origin, "world:raycast", "origin");
            if (!from)
                return result;

            glm::vec3 dir{};
            if (!ReadVec3(direction, dir, "world:raycast", "direction"))
                return result;

            float distance = kRayCastDefaultMaxDistance;
            if (Given(maxDistance))
            {
                double number = 0.0;
                if (!ReadNumber(*maxDistance, number, "world:raycast", "maxDistance"))
                    return result;
                distance = static_cast<float>(number);
            }

            const PhysicsWorld *physics = world.TryResource<PhysicsWorld>();
            if (physics == nullptr)
                return result;

            const std::optional<WorldPosition> position = WorldPosition::FromMeters(*from);
            if (!position)
                return result;

            RayFilter filter;
            std::vector<BodyHandle> excluded;
            sol::object optionsGiven = Given(options) ? *options : sol::make_object(lua, sol::lua_nil);
            if (optionsGiven.get_type() != sol::type::nil)
            {
                if (!optionsGiven.is<sol::table>())
                {
                    MIR_LOG_ERROR("script: world:raycast needs a table for options");
                    return result;
                }
                if (!ReadCastOptions(world, lua, optionsGiven.as<sol::table>(), filter, excluded))
                    return result;
            }
            else
            {
                // no options: the default still keeps the caster out of its own cast
                ExcludeCaster(world, lua, excluded);
                filter.mExclude = excluded;
            }

            const std::optional<RayHit> hit = physics->RayCast(*position, dir, distance, filter);
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

        const PhysicsLayerTable &LayerTableOf(World &world)
        {
            static const PhysicsLayerTable kDefault;
            const PhysicsWorld *physics = world.TryResource<PhysicsWorld>();
            return physics != nullptr ? physics->LayerTable() : kDefault;
        }

        // A layer as a script writes it: a name from the project's table or an index 0 to 31. nullopt, logged,
        // for anything else, with the layers that do exist.
        std::optional<int> ResolveLayer(const PhysicsLayerTable &table, const sol::object &value, std::string_view call)
        {
            if (value.get_type() == sol::type::number)
            {
                const double number = value.as<double>();
                if (std::floor(number) == number && number >= 0.0 && number < kPhysicsLayerCount)
                    return static_cast<int>(number);
                MIR_LOG_ERROR("script: {} needs a layer index from 0 to {}, not {}", call, kPhysicsLayerCount - 1, number);
                return std::nullopt;
            }

            if (value.get_type() == sol::type::string)
            {
                const std::string name = value.as<std::string>();
                if (const std::optional<int> index = table.IndexOf(name))
                    return index;

                std::string known;
                for (int i = 0; i < kPhysicsLayerCount; ++i)
                {
                    if (!table.names[i].empty())
                        known += (known.empty() ? "" : ", ") + table.names[i];
                }
                MIR_LOG_ERROR("script: {} has no layer named '{}'; the layers are {}", call, name, known);
                return std::nullopt;
            }

            MIR_LOG_ERROR("script: {} needs a layer name or an index from 0 to {}", call, kPhysicsLayerCount - 1);
            return std::nullopt;
        }

        bool WorldSetLayer(sol::this_state state, World &world, Entity entity, const sol::object &layer)
        {
            const std::optional<int> index = ResolveLayer(LayerTableOf(world), layer, "world:set_layer");
            if (!index)
                return false;
            return WriteRigidBodyField(state, world, entity, "layer", sol::make_object(sol::state_view(state), *index));
        }

        sol::object WorldLayerIndex(sol::this_state state, World &world, std::string_view name)
        {
            sol::state_view lua(state);
            const std::optional<int> index = LayerTableOf(world).IndexOf(name);
            return index ? sol::make_object(lua, *index) : sol::make_object(lua, sol::lua_nil);
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
            const std::optional<WorldVec3> at = ReadWorldVec3(position, "world:teleport", "position");
            const std::optional<WorldPosition> target = at ? WorldPosition::FromMeters(*at) : std::nullopt;
            if (!target)
                return false;

            std::optional<glm::quat> turned;
            if (Given(rotation))
            {
                glm::quat q;
                if (!ReadQuat(*rotation, q, "world:teleport", "rotation"))
                    return false;
                turned = q;
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
            // `axis` is the input bridge's world:axis(action), so the capsule's is spelled out
            const std::string snake = field.mName == "axis" ? std::string("capsule_axis") : SnakeCase(field.mName);
            worldType.set_function(snake,
                [name = std::string(field.mName)](sol::this_state state, World &world, Entity entity)
                { return ReadRigidBodyField(state, world, entity, name); });
            // a layer is set by name or index, below
            if (field.mName == "layer")
                continue;
            worldType.set_function("set_" + snake,
                [name = std::string(field.mName)](sol::this_state state, World &world, Entity entity, const sol::object &value)
                { return WriteRigidBodyField(state, world, entity, name, value); });
        }

        worldType.set_function("set_layer", &WorldSetLayer);
        worldType.set_function("layer_index", &WorldLayerIndex);

        RegisterEnum(lua, "PhysicsMotion", kPhysicsMotionNames);
        RegisterEnum(lua, "PhysicsShape", kPhysicsShapeNames);
        RegisterEnum(lua, "PhysicsAxis", kPhysicsAxisNames);
        worldType.set_function("collisions", &WorldCollisions);
    }
}
