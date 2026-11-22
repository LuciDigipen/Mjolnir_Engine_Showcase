/**
 * @file FlightBindings.cpp
 * @author Jackey Cen
 * @brief Extends the World usertype with aircraft telemetry, wind, reset, touchdown and control calls.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <flight_script/FlightBindings.h>

#include <script/BindingApi.h>
#include <script/BindingHelpers.h>

#include <core/ecs/World.h>
#include <flight/AircraftRuntime.h>
#include <flight/FlightLandingGear.h>
#include <flight/FlightReset.h>
#include <flight/FlightTelemetry.h>
#include <flight/components/Aircraft.h>

#include <sol/sol.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include <cstddef>
#include <optional>
#include <span>

namespace mir
{
    namespace
    {
        sol::table ForcesToLua(sol::state_view lua, const TelemetryForces &forces)
        {
            sol::table out = lua.create_table();
            out["force"] = Vec3ToLua(lua, forces.force);
            out["moment"] = Vec3ToLua(lua, forces.moment);
            return out;
        }

        sol::table TelemetryToLua(sol::state_view lua, const FlightTelemetry &telemetry)
        {
            sol::table out = lua.create_table();
            out["true_airspeed"] = telemetry.trueAirspeed;
            out["indicated_airspeed"] = telemetry.indicatedAirspeed;
            out["altitude"] = telemetry.altitude;
            out["height_above_ground"] = telemetry.heightAboveGround;
            out["has_ground"] = telemetry.hasGround;
            out["vertical_speed"] = telemetry.verticalSpeed;
            out["heading"] = glm::degrees(telemetry.heading);
            out["pitch"] = glm::degrees(telemetry.pitch);
            out["roll"] = glm::degrees(telemetry.roll);
            out["angle_of_attack"] = glm::degrees(telemetry.angleOfAttack);
            out["sideslip"] = glm::degrees(telemetry.sideslip);
            out["dynamic_pressure"] = telemetry.dynamicPressure;
            out["mach"] = telemetry.mach;
            out["load_factor"] = telemetry.loadFactor;
            out["stall_margin"] = glm::degrees(telemetry.stallMargin);
            out["stall_warning"] = telemetry.stallWarning;
            out["throttle"] = telemetry.throttle;

            sol::table rpm = lua.create_table();
            for (std::size_t i = 0; i < telemetry.engineCount; ++i)
                rpm[i + 1] = telemetry.rpm[i];
            out["rpm"] = rpm;
            out["engine_count"] = static_cast<int>(telemetry.engineCount);

            out["flaps"] = glm::degrees(telemetry.flaps);
            out["trim"] = glm::degrees(telemetry.trim);
            out["elevator"] = glm::degrees(telemetry.elevator);
            out["aileron"] = glm::degrees(telemetry.aileron);
            out["rudder"] = glm::degrees(telemetry.rudder);

            sol::table compression = lua.create_table();
            for (std::size_t i = 0; i < telemetry.gearCount; ++i)
                compression[i + 1] = telemetry.gearCompression[i];
            out["gear_compression"] = compression;
            out["gear_count"] = static_cast<int>(telemetry.gearCount);

            out["weight_on_wheels"] = telemetry.weightOnWheels;
            out["aero_forces"] = ForcesToLua(lua, telemetry.aeroForces);
            out["propulsion_forces"] = ForcesToLua(lua, telemetry.propulsionForces);
            out["gear_forces"] = ForcesToLua(lua, telemetry.gearForces);
            return out;
        }

        sol::object WorldAircraftTelemetry(World &world, Entity entity, sol::this_state state)
        {
            sol::state_view lua(state);
            const Aircraft *aircraft = world.GetComponent<Aircraft>(entity);
            if (aircraft == nullptr)
                return sol::make_object(lua, sol::lua_nil);

            return sol::make_object(lua, TelemetryToLua(lua, aircraft->runtime.telemetry));
        }

        bool WorldSetAircraftWind(World &world, Entity entity, const sol::object &wind)
        {
            glm::vec3 vector{};
            if (!ReadVec3(wind, vector, "world:set_aircraft_wind", "wind"))
                return false;

            Aircraft *aircraft = world.GetComponent<Aircraft>(entity);
            if (aircraft == nullptr)
                return false;

            aircraft->runtime.additionalWind += vector;
            return true;
        }

        // The position and rotation are the forms world:teleport accepts.
        bool WorldResetAircraft(World &world, Entity entity, const sol::object &position,
            const sol::object &rotation, const sol::object &airspeed, const sol::object &throttle)
        {
            const std::optional<WorldVec3> at = ReadWorldVec3(position, "world:reset_aircraft", "position");
            const std::optional<WorldPosition> target = at ? WorldPosition::FromMeters(*at) : std::nullopt;
            if (!target)
                return false;

            glm::quat turned(1.0f, 0.0f, 0.0f, 0.0f);
            if (rotation.get_type() != sol::type::nil &&
                !ReadQuat(rotation, turned, "world:reset_aircraft", "rotation"))
                return false;

            double speed = 0.0;
            double throttleValue = 0.0;
            if (!ReadNumber(airspeed, speed, "world:reset_aircraft", "airspeed") ||
                !ReadNumber(throttle, throttleValue, "world:reset_aircraft", "throttle"))
                return false;

            return ResetAircraft(world, entity, *target, turned, static_cast<float>(speed),
                static_cast<float>(throttleValue));
        }

        sol::object WorldAircraftTouchdowns(World &world, Entity entity, sol::this_state state)
        {
            sol::state_view lua(state);
            const Aircraft *aircraft = world.GetComponent<Aircraft>(entity);
            if (aircraft == nullptr)
                return sol::make_object(lua, sol::lua_nil);

            sol::table out = lua.create_table();
            const std::span<const Touchdown> events = AircraftTouchdowns(*aircraft);
            int index = 0;
            for (const Touchdown &event : events)
            {
                sol::table entry = lua.create_table();
                entry["gear"] = event.gearIndex + 1;
                entry["sink_rate"] = event.sinkRate;
                entry["compression"] = event.compression;
                out[++index] = entry;
            }
            return sol::make_object(lua, out);
        }

        sol::object WorldAircraftControls(World &world, Entity entity, sol::this_state state)
        {
            sol::state_view lua(state);
            const Aircraft *aircraft = world.GetComponent<Aircraft>(entity);
            if (aircraft == nullptr)
                return sol::make_object(lua, sol::lua_nil);

            const FlightControls &controls = aircraft->runtime.controls;
            const ActuatorState &actuators = aircraft->runtime.actuators;

            sol::table out = lua.create_table();
            out["pitch"] = controls.pitch;
            out["roll"] = controls.roll;
            out["yaw"] = controls.yaw;
            out["throttle_up"] = controls.throttleUp;
            out["throttle_down"] = controls.throttleDown;
            out["brake"] = controls.brake;
            out["throttle"] = actuators.throttle;
            out["flaps_detent"] = actuators.flapDetent;
            out["trim_deg"] = glm::degrees(actuators.trim);
            return sol::make_object(lua, out);
        }
    }

    void RegisterFlightBindings(sol::state &lua)
    {
        sol::table worldType = lua["World"];
        worldType.set_function("aircraft_telemetry", &WorldAircraftTelemetry);
        worldType.set_function("set_aircraft_wind", &WorldSetAircraftWind);
        worldType.set_function("reset_aircraft", &WorldResetAircraft);
        worldType.set_function("aircraft_touchdowns", &WorldAircraftTouchdowns);
        worldType.set_function("aircraft_controls", &WorldAircraftControls);
    }
}
