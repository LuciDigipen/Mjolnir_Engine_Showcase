/**
 * @file FlightDebugRows.cpp
 * @author Jackey Cen
 * @brief The label and value rows the Flight Debug window draws, built without ImGui.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight_debug/FlightDebugRows.h>

#include <glm/gtc/constants.hpp>
#include <glm/vec3.hpp>

#include <format>
#include <string>
#include <vector>

namespace mir
{
    namespace
    {
        std::string FormatVec3(const glm::vec3 &value, const char *unit = "")
        {
            const std::string numbers = std::format("({:.2f}, {:.2f}, {:.2f})", value.x, value.y, value.z);
            return unit[0] == '\0' ? numbers : numbers + ' ' + unit;
        }

        std::string FormatDegrees(float radians)
        {
            return std::format("{:.1f} deg", FlightDebugDegrees(radians));
        }

        std::string FormatDetents(const std::vector<float> &detents)
        {
            std::string value;
            for (const float detent : detents)
            {
                if (!value.empty())
                    value += ", ";
                value += std::format("{:.1f}", FlightDebugDegrees(detent));
            }
            return value.empty() ? "none" : value + " deg";
        }
    }

    bool FlightDebugTierAvailable(const AircraftConfig &config, AerodynamicTier tier)
    {
        return HasAerodynamicProfile(config.aero, tier);
    }

    bool SetFlightDebugTier(Aircraft &aircraft, const AircraftConfig *config, AerodynamicTier tier)
    {
        aircraft.tier = tier;
        return config != nullptr && FlightDebugTierAvailable(*config, tier);
    }

    bool FlightDebugWindowVisible(bool editorBuild, bool playWorldActive, bool toggledOn)
    {
        return toggledOn && (!editorBuild || playWorldActive);
    }

    float FlightDebugDegrees(float radians) { return radians * (180.0f / glm::pi<float>()); }

    std::vector<FlightDebugRow> BuildFlightDebugConfigRows(const AircraftConfig &config)
    {
        std::vector<FlightDebugRow> rows;
        rows.push_back({ "Mass", std::format("{:.2f} kg", config.mass) });
        rows.push_back({ "Inertia diagonal", FormatVec3(config.inertiaDiagonal, "kg m^2") });
        rows.push_back({ "Inertia products", FormatVec3(config.inertiaProducts, "kg m^2") });
        rows.push_back({ "Centre of mass", FormatVec3(config.centerOfMass, "m") });
        rows.push_back({ "Reference area", std::format("{:.2f} m^2", config.reference.area) });
        rows.push_back({ "Reference span", std::format("{:.2f} m", config.reference.span) });
        rows.push_back({ "Reference chord", std::format("{:.2f} m", config.reference.chord) });

        const AircraftConfig::Actuators &actuators = config.actuators;
        rows.push_back({ "Elevator travel", FormatDegrees(actuators.elevator.maxDeflection) });
        rows.push_back({ "Elevator rate", std::format("{:.1f} deg/s", FlightDebugDegrees(actuators.elevator.rate)) });
        rows.push_back({ "Aileron travel", FormatDegrees(actuators.aileron.maxDeflection) });
        rows.push_back({ "Aileron rate", std::format("{:.1f} deg/s", FlightDebugDegrees(actuators.aileron.rate)) });
        rows.push_back({ "Rudder travel", FormatDegrees(actuators.rudder.maxDeflection) });
        rows.push_back({ "Rudder rate", std::format("{:.1f} deg/s", FlightDebugDegrees(actuators.rudder.rate)) });
        rows.push_back({ "Flap detents", FormatDetents(actuators.flaps.detents) });
        rows.push_back({ "Flap rate", std::format("{:.1f} deg/s", FlightDebugDegrees(actuators.flaps.rate)) });
        rows.push_back({ "Trim step", FormatDegrees(actuators.trim.step) });
        rows.push_back({ "Trim limit", FormatDegrees(actuators.trim.max) });
        rows.push_back({ "Engines", std::format("{}", config.engines.size()) });
        rows.push_back({ "Gear points", std::format("{}", config.gear.size()) });
        return rows;
    }

    std::vector<FlightDebugRow> BuildFlightDebugTelemetryRows(const AircraftRuntime &runtime)
    {
        const FlightTelemetry &telemetry = runtime.telemetry;
        std::vector<FlightDebugRow> rows;
        rows.push_back({ "True airspeed", std::format("{:.2f} m/s", telemetry.trueAirspeed) });
        rows.push_back({ "Indicated airspeed", std::format("{:.2f} m/s", telemetry.indicatedAirspeed) });
        rows.push_back({ "Altitude", std::format("{:.1f} m", telemetry.altitude) });
        rows.push_back({ "Height above ground",
            telemetry.hasGround ? std::format("{:.1f} m", telemetry.heightAboveGround) : std::string("n/a") });
        rows.push_back({ "Vertical speed", std::format("{:.2f} m/s", telemetry.verticalSpeed) });
        rows.push_back({ "Heading", FormatDegrees(telemetry.heading) });
        rows.push_back({ "Pitch", FormatDegrees(telemetry.pitch) });
        rows.push_back({ "Roll", FormatDegrees(telemetry.roll) });
        rows.push_back({ "Angle of attack", FormatDegrees(telemetry.angleOfAttack) });
        rows.push_back({ "Sideslip", FormatDegrees(telemetry.sideslip) });
        rows.push_back({ "Dynamic pressure", std::format("{:.1f} Pa", telemetry.dynamicPressure) });
        rows.push_back({ "Mach", std::format("{:.3f}", telemetry.mach) });
        rows.push_back({ "Load factor", std::format("{:.2f} g", telemetry.loadFactor) });
        rows.push_back({ "Stall margin", FormatDegrees(telemetry.stallMargin), telemetry.stallWarning });
        rows.push_back({ "Throttle", std::format("{:.0f} %", telemetry.throttle * 100.0f) });

        for (std::size_t i = 0; i < telemetry.engineCount; ++i)
            rows.push_back({ std::format("RPM {}", i + 1), std::format("{:.0f}", telemetry.rpm[i]) });

        rows.push_back({ "Flaps", FormatDegrees(telemetry.flaps) });
        rows.push_back({ "Trim", FormatDegrees(telemetry.trim) });
        rows.push_back({ "Elevator", FormatDegrees(telemetry.elevator) });
        rows.push_back({ "Aileron", FormatDegrees(telemetry.aileron) });
        rows.push_back({ "Rudder", FormatDegrees(telemetry.rudder) });

        for (std::size_t i = 0; i < telemetry.gearCount; ++i)
            rows.push_back({ std::format("Gear compression {}", i + 1),
                std::format("{:.3f} m", telemetry.gearCompression[i]) });

        rows.push_back({ "Weight on wheels", telemetry.weightOnWheels ? "yes" : "no" });
        return rows;
    }

    std::vector<FlightDebugRow> BuildFlightDebugForceRows(const AircraftRuntime &runtime)
    {
        const ForceMomentSources &sources = runtime.aero.sources;
        std::vector<FlightDebugRow> rows;
        rows.push_back({ "Aero lift", FormatVec3(sources.lift, "N") });
        rows.push_back({ "Aero drag", FormatVec3(sources.drag, "N") });
        rows.push_back({ "Aero side", FormatVec3(sources.side, "N") });
        rows.push_back({ "Aero control moment", FormatVec3(sources.controlMoment, "N m") });
        rows.push_back({ "Aero damping moment", FormatVec3(sources.dampingMoment, "N m") });
        rows.push_back({ "Aero stability moment", FormatVec3(sources.stabilityMoment, "N m") });
        rows.push_back({ "Propulsion force", FormatVec3(runtime.propulsion.force, "N") });
        rows.push_back({ "Propulsion moment", FormatVec3(runtime.propulsion.moment, "N m") });
        rows.push_back({ "Gear force", FormatVec3(runtime.gear.force, "N") });
        rows.push_back({ "Gear moment", FormatVec3(runtime.gear.moment, "N m") });
        return rows;
    }
}
