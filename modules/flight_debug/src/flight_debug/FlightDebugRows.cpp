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
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        constexpr char kAxisNames[] = "XYZ";

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

        void AddScalar(std::vector<FlightDebugField> &fields, std::string group, std::string label,
            std::function<float(const AircraftConfig &)> get, std::function<void(AircraftConfig &, float)> set,
            bool degrees = false)
        {
            fields.push_back(
                FlightDebugField{ std::move(group), std::move(label), std::move(get), std::move(set), degrees });
        }

        void AddVector(std::vector<FlightDebugField> &fields, const std::string &group, const std::string &label,
            const std::function<glm::vec3(const AircraftConfig &)> &get,
            const std::function<void(AircraftConfig &, const glm::vec3 &)> &set)
        {
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                AddScalar(fields, group, std::format("{} {}", label, kAxisNames[axis]),
                    [get, axis](const AircraftConfig &config) { return get(config)[axis]; },
                    [get, set, axis](AircraftConfig &config, float value)
                    {
                        glm::vec3 vector = get(config);
                        vector[axis] = value;
                        set(config, vector);
                    });
            }
        }

        void AddActuator(std::vector<FlightDebugField> &fields, const std::string &group, const std::string &label,
            AircraftConfig::Actuator AircraftConfig::Actuators::*member)
        {
            AddScalar(fields, group, label + " travel",
                [member](const AircraftConfig &config) { return (config.actuators.*member).maxDeflection; },
                [member](AircraftConfig &config, float value) { (config.actuators.*member).maxDeflection = value; },
                true);
            AddScalar(fields, group, label + " rate",
                [member](const AircraftConfig &config) { return (config.actuators.*member).rate; },
                [member](AircraftConfig &config, float value) { (config.actuators.*member).rate = value; }, true);
            AddScalar(fields, group, label + " delay",
                [member](const AircraftConfig &config) { return (config.actuators.*member).delay; },
                [member](AircraftConfig &config, float value) { (config.actuators.*member).delay = value; });
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

    std::vector<FlightDebugRow> BuildFlightDebugL3Rows(const AircraftConfig &config, const AircraftRuntime &runtime)
    {
        std::vector<FlightDebugRow> rows;
        if (!config.aero.l3.has_value())
            return rows;

        const FlightTelemetry &telemetry = runtime.telemetry;
        const L3Sample sample = SampleL3(*config.aero.l3, telemetry.angleOfAttack, telemetry.sideslip,
            telemetry.mach, runtime.actuators.flaps);
        rows.push_back({ "Cell alpha", std::format("{}", sample.alphaIndex) });
        rows.push_back({ "Cell beta", std::format("{}", sample.betaIndex) });
        rows.push_back({ "Cell Mach", std::format("{}", sample.machIndex) });
        rows.push_back({ "Cell flap", std::format("{}", sample.flapIndex) });
        rows.push_back({ "Base CX", std::format("{:.4f}", sample.base.CX) });
        rows.push_back({ "Base CY", std::format("{:.4f}", sample.base.CY) });
        rows.push_back({ "Base CZ", std::format("{:.4f}", sample.base.CZ) });
        rows.push_back({ "Base Cl", std::format("{:.4f}", sample.base.Cl) });
        rows.push_back({ "Base Cm", std::format("{:.4f}", sample.base.Cm) });
        rows.push_back({ "Base Cn", std::format("{:.4f}", sample.base.Cn) });
        rows.push_back({ "Out of table", sample.outOfTable ? "yes" : "no", sample.outOfTable });
        return rows;
    }

    std::vector<FlightDebugField> BuildFlightDebugEditableFields(const AircraftConfig &config)
    {
        std::vector<FlightDebugField> fields;

        AddScalar(fields, "Body", "Mass", [](const AircraftConfig &c) { return c.mass; },
            [](AircraftConfig &c, float value) { c.mass = value; });
        AddVector(fields, "Body", "Inertia diagonal", [](const AircraftConfig &c) { return c.inertiaDiagonal; },
            [](AircraftConfig &c, const glm::vec3 &value) { c.inertiaDiagonal = value; });
        AddVector(fields, "Body", "Inertia product", [](const AircraftConfig &c) { return c.inertiaProducts; },
            [](AircraftConfig &c, const glm::vec3 &value) { c.inertiaProducts = value; });
        AddVector(fields, "Body", "Centre of mass", [](const AircraftConfig &c) { return c.centerOfMass; },
            [](AircraftConfig &c, const glm::vec3 &value) { c.centerOfMass = value; });
        AddScalar(fields, "Body", "Reference area", [](const AircraftConfig &c) { return c.reference.area; },
            [](AircraftConfig &c, float value) { c.reference.area = value; });
        AddScalar(fields, "Body", "Reference span", [](const AircraftConfig &c) { return c.reference.span; },
            [](AircraftConfig &c, float value) { c.reference.span = value; });
        AddScalar(fields, "Body", "Reference chord", [](const AircraftConfig &c) { return c.reference.chord; },
            [](AircraftConfig &c, float value) { c.reference.chord = value; });
        AddScalar(fields, "Body", "Throttle rate", [](const AircraftConfig &c) { return c.throttleRate; },
            [](AircraftConfig &c, float value) { c.throttleRate = value; });
        AddScalar(fields, "Body", "Stall warning margin",
            [](const AircraftConfig &c) { return c.stallWarningMargin; },
            [](AircraftConfig &c, float value) { c.stallWarningMargin = value; }, true);

        AddVector(fields, "Camera", "Eye", [](const AircraftConfig &c) { return c.camera.eye; },
            [](AircraftConfig &c, const glm::vec3 &value) { c.camera.eye = value; });
        AddScalar(fields, "Camera", "Chase distance",
            [](const AircraftConfig &c) { return c.camera.chaseDistance; },
            [](AircraftConfig &c, float value) { c.camera.chaseDistance = value; });
        AddScalar(fields, "Camera", "Chase height", [](const AircraftConfig &c) { return c.camera.chaseHeight; },
            [](AircraftConfig &c, float value) { c.camera.chaseHeight = value; });

        AddActuator(fields, "Actuators", "Elevator", &AircraftConfig::Actuators::elevator);
        AddActuator(fields, "Actuators", "Aileron", &AircraftConfig::Actuators::aileron);
        AddActuator(fields, "Actuators", "Rudder", &AircraftConfig::Actuators::rudder);
        AddScalar(fields, "Actuators", "Flap rate", [](const AircraftConfig &c) { return c.actuators.flaps.rate; },
            [](AircraftConfig &c, float value) { c.actuators.flaps.rate = value; }, true);
        AddScalar(fields, "Actuators", "Trim step", [](const AircraftConfig &c) { return c.actuators.trim.step; },
            [](AircraftConfig &c, float value) { c.actuators.trim.step = value; }, true);
        AddScalar(fields, "Actuators", "Trim max", [](const AircraftConfig &c) { return c.actuators.trim.max; },
            [](AircraftConfig &c, float value) { c.actuators.trim.max = value; }, true);

        for (std::size_t i = 0; i < config.engines.size(); ++i)
        {
            const std::string group = std::format("Engine {}", i + 1);
            AddScalar(fields, group, "Idle rpm",
                [i](const AircraftConfig &c) { return c.engines[i].idleRpm; },
                [i](AircraftConfig &c, float value) { c.engines[i].idleRpm = value; });
            AddScalar(fields, group, "Max rpm", [i](const AircraftConfig &c) { return c.engines[i].maxRpm; },
                [i](AircraftConfig &c, float value) { c.engines[i].maxRpm = value; });
            AddScalar(fields, group, "rpm tau", [i](const AircraftConfig &c) { return c.engines[i].rpmTau; },
                [i](AircraftConfig &c, float value) { c.engines[i].rpmTau = value; });
            AddVector(fields, group, "Position",
                [i](const AircraftConfig &c) { return c.engines[i].position; },
                [i](AircraftConfig &c, const glm::vec3 &value) { c.engines[i].position = value; });
            AddVector(fields, group, "Direction",
                [i](const AircraftConfig &c) { return c.engines[i].direction; },
                [i](AircraftConfig &c, const glm::vec3 &value) { c.engines[i].direction = value; });
        }

        for (std::size_t i = 0; i < config.gear.size(); ++i)
        {
            const std::string group = std::format("Gear {}", i + 1);
            AddVector(fields, group, "Mount", [i](const AircraftConfig &c) { return c.gear[i].mount; },
                [i](AircraftConfig &c, const glm::vec3 &value) { c.gear[i].mount = value; });
            AddScalar(fields, group, "Rest length", [i](const AircraftConfig &c) { return c.gear[i].restLength; },
                [i](AircraftConfig &c, float value) { c.gear[i].restLength = value; });
            AddScalar(fields, group, "Stiffness", [i](const AircraftConfig &c) { return c.gear[i].stiffness; },
                [i](AircraftConfig &c, float value) { c.gear[i].stiffness = value; });
            AddScalar(fields, group, "Damping", [i](const AircraftConfig &c) { return c.gear[i].damping; },
                [i](AircraftConfig &c, float value) { c.gear[i].damping = value; });
            AddScalar(fields, group, "Rolling friction",
                [i](const AircraftConfig &c) { return c.gear[i].rollingFriction; },
                [i](AircraftConfig &c, float value) { c.gear[i].rollingFriction = value; });
            AddScalar(fields, group, "Side friction",
                [i](const AircraftConfig &c) { return c.gear[i].sideFriction; },
                [i](AircraftConfig &c, float value) { c.gear[i].sideFriction = value; });
            AddScalar(fields, group, "Brake", [i](const AircraftConfig &c) { return c.gear[i].brake; },
                [i](AircraftConfig &c, float value) { c.gear[i].brake = value; });
            AddScalar(fields, group, "Max steer", [i](const AircraftConfig &c) { return c.gear[i].maxSteer; },
                [i](AircraftConfig &c, float value) { c.gear[i].maxSteer = value; }, true);
        }

        if (config.aero.l1.has_value())
        {
            AddScalar(fields, "Aero L1", "CL", [](const AircraftConfig &c) { return c.aero.l1->CL; },
                [](AircraftConfig &c, float value) { c.aero.l1->CL = value; });
            AddScalar(fields, "Aero L1", "CD", [](const AircraftConfig &c) { return c.aero.l1->CD; },
                [](AircraftConfig &c, float value) { c.aero.l1->CD = value; });
            AddScalar(fields, "Aero L1", "Ka", [](const AircraftConfig &c) { return c.aero.l1->Ka; },
                [](AircraftConfig &c, float value) { c.aero.l1->Ka = value; });
            AddScalar(fields, "Aero L1", "Ke", [](const AircraftConfig &c) { return c.aero.l1->Ke; },
                [](AircraftConfig &c, float value) { c.aero.l1->Ke = value; });
            AddScalar(fields, "Aero L1", "Kr", [](const AircraftConfig &c) { return c.aero.l1->Kr; },
                [](AircraftConfig &c, float value) { c.aero.l1->Kr = value; });
            AddScalar(fields, "Aero L1", "Kp", [](const AircraftConfig &c) { return c.aero.l1->Kp; },
                [](AircraftConfig &c, float value) { c.aero.l1->Kp = value; });
            AddScalar(fields, "Aero L1", "Kq", [](const AircraftConfig &c) { return c.aero.l1->Kq; },
                [](AircraftConfig &c, float value) { c.aero.l1->Kq = value; });
            AddScalar(fields, "Aero L1", "Kn", [](const AircraftConfig &c) { return c.aero.l1->Kn; },
                [](AircraftConfig &c, float value) { c.aero.l1->Kn = value; });
        }

        if (config.aero.l2.has_value())
        {
            AddScalar(fields, "Aero L2", "CL0", [](const AircraftConfig &c) { return c.aero.l2->CL0; },
                [](AircraftConfig &c, float value) { c.aero.l2->CL0 = value; });
            AddScalar(fields, "Aero L2", "CLalpha", [](const AircraftConfig &c) { return c.aero.l2->CLalpha; },
                [](AircraftConfig &c, float value) { c.aero.l2->CLalpha = value; });
            AddScalar(fields, "Aero L2", "CD0", [](const AircraftConfig &c) { return c.aero.l2->CD0; },
                [](AircraftConfig &c, float value) { c.aero.l2->CD0 = value; });
            AddScalar(fields, "Aero L2", "k", [](const AircraftConfig &c) { return c.aero.l2->k; },
                [](AircraftConfig &c, float value) { c.aero.l2->k = value; });
            AddScalar(fields, "Aero L2", "Cm0", [](const AircraftConfig &c) { return c.aero.l2->Cm0; },
                [](AircraftConfig &c, float value) { c.aero.l2->Cm0 = value; });
            AddScalar(fields, "Aero L2", "Cmalpha", [](const AircraftConfig &c) { return c.aero.l2->Cmalpha; },
                [](AircraftConfig &c, float value) { c.aero.l2->Cmalpha = value; });
            AddScalar(fields, "Aero L2", "Cmq", [](const AircraftConfig &c) { return c.aero.l2->Cmq; },
                [](AircraftConfig &c, float value) { c.aero.l2->Cmq = value; });
            AddScalar(fields, "Aero L2", "Cmde", [](const AircraftConfig &c) { return c.aero.l2->Cmde; },
                [](AircraftConfig &c, float value) { c.aero.l2->Cmde = value; });
            AddScalar(fields, "Aero L2", "CYbeta", [](const AircraftConfig &c) { return c.aero.l2->CYbeta; },
                [](AircraftConfig &c, float value) { c.aero.l2->CYbeta = value; });
            AddScalar(fields, "Aero L2", "CYdr", [](const AircraftConfig &c) { return c.aero.l2->CYdr; },
                [](AircraftConfig &c, float value) { c.aero.l2->CYdr = value; });
            AddScalar(fields, "Aero L2", "Clbeta", [](const AircraftConfig &c) { return c.aero.l2->Clbeta; },
                [](AircraftConfig &c, float value) { c.aero.l2->Clbeta = value; });
            AddScalar(fields, "Aero L2", "Clp", [](const AircraftConfig &c) { return c.aero.l2->Clp; },
                [](AircraftConfig &c, float value) { c.aero.l2->Clp = value; });
            AddScalar(fields, "Aero L2", "Clda", [](const AircraftConfig &c) { return c.aero.l2->Clda; },
                [](AircraftConfig &c, float value) { c.aero.l2->Clda = value; });
            AddScalar(fields, "Aero L2", "Cnbeta", [](const AircraftConfig &c) { return c.aero.l2->Cnbeta; },
                [](AircraftConfig &c, float value) { c.aero.l2->Cnbeta = value; });
            AddScalar(fields, "Aero L2", "Cnr", [](const AircraftConfig &c) { return c.aero.l2->Cnr; },
                [](AircraftConfig &c, float value) { c.aero.l2->Cnr = value; });
            AddScalar(fields, "Aero L2", "Cndr", [](const AircraftConfig &c) { return c.aero.l2->Cndr; },
                [](AircraftConfig &c, float value) { c.aero.l2->Cndr = value; });
            AddScalar(fields, "Aero L2", "Cnda", [](const AircraftConfig &c) { return c.aero.l2->Cnda; },
                [](AircraftConfig &c, float value) { c.aero.l2->Cnda = value; });
            AddScalar(fields, "Aero L2", "Min airspeed",
                [](const AircraftConfig &c) { return c.aero.l2->minAirspeed; },
                [](AircraftConfig &c, float value) { c.aero.l2->minAirspeed = value; });

            AddScalar(fields, "Aero L2 stall", "Positive",
                [](const AircraftConfig &c) { return c.aero.l2->stall.positive; },
                [](AircraftConfig &c, float value) { c.aero.l2->stall.positive = value; }, true);
            AddScalar(fields, "Aero L2 stall", "Negative",
                [](const AircraftConfig &c) { return c.aero.l2->stall.negative; },
                [](AircraftConfig &c, float value) { c.aero.l2->stall.negative = value; }, true);
            AddScalar(fields, "Aero L2 stall", "Blend", [](const AircraftConfig &c) { return c.aero.l2->stall.blend; },
                [](AircraftConfig &c, float value) { c.aero.l2->stall.blend = value; }, true);
            AddScalar(fields, "Aero L2 stall", "Control scale",
                [](const AircraftConfig &c) { return c.aero.l2->stall.controlScale; },
                [](AircraftConfig &c, float value) { c.aero.l2->stall.controlScale = value; });
            AddScalar(fields, "Aero L2 stall", "Stability scale",
                [](const AircraftConfig &c) { return c.aero.l2->stall.stabilityScale; },
                [](AircraftConfig &c, float value) { c.aero.l2->stall.stabilityScale = value; });
        }

        return fields;
    }

    std::vector<FlightDebugRow> BuildFlightDebugTableRows(const AircraftConfig &config)
    {
        std::vector<FlightDebugRow> rows;
        rows.push_back({ "Flap detents", std::format("{} detents", config.actuators.flaps.detents.size()) });

        for (std::size_t i = 0; i < config.engines.size(); ++i)
        {
            rows.push_back({ std::format("Engine {} static thrust", i + 1),
                std::format("{} points", config.engines[i].staticThrust.size()) });
            rows.push_back({ std::format("Engine {} speed factor", i + 1),
                std::format("{} points", config.engines[i].speedFactor.size()) });
        }

        if (config.aero.l2.has_value())
        {
            rows.push_back({ "L2 post-stall", std::format("{} rows", config.aero.l2->stall.postStall.size()) });
            rows.push_back({ "L2 flaps", std::format("{} rows", config.aero.l2->flaps.size()) });
        }

        if (config.aero.l3.has_value())
        {
            const L3Grid &grid = config.aero.l3->grid;
            rows.push_back({ "L3 grid", std::format("{} x {} x {} x {}", grid.alpha.size(), grid.beta.size(),
                grid.mach.size(), grid.flaps.size()) });
            rows.push_back({ "L3 base cells", std::format("{} cells", config.aero.l3->base.CX.size()) });
        }

        rows.push_back({ "Nodes", std::format("{} entries", config.nodes.size()) });
        return rows;
    }
}
