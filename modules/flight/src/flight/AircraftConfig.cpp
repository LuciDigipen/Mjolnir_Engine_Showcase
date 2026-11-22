/**
 * @file AircraftConfig.cpp
 * @author Jackey Cen
 * @brief Parses, decodes and writes the *.aircraft.json configuration asset.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/AircraftConfig.h>

#include <assets/AssetBlob.h>

#include <nlohmann/json.hpp>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <charconv>
#include <cmath>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        using Json = nlohmann::ordered_json;

        bool IsKnownKey(std::string_view key)
        {
            return key == "version" || key == "mass" || key == "inertia" || key == "centerOfMass"
                || key == "reference" || key == "camera" || key == "nodes"
                || key == "throttleRate" || key == "stallWarningMarginDeg" || key == "actuators"
                || key == "engines" || key == "gear" || key == "aero";
        }

        bool ReadNumber(const Json &value, const std::string &path, float &out, std::string &error)
        {
            if (!value.is_number())
            {
                error = std::format("{}: expected a number", path);
                return false;
            }

            const double number = value.get<double>();
            if (!std::isfinite(number) || !std::isfinite(static_cast<float>(number)))
            {
                error = std::format("{}: must be finite", path);
                return false;
            }

            out = static_cast<float>(number);
            return true;
        }

        bool ReadPositiveNumber(const Json &value, const std::string &path, float &out, std::string &error)
        {
            if (!ReadNumber(value, path, out, error))
                return false;
            if (out <= 0.0f)
            {
                error = std::format("{}: must be > 0", path);
                return false;
            }
            return true;
        }

        bool ReadNonNegativeNumber(const Json &value, const std::string &path, float &out, std::string &error)
        {
            if (!ReadNumber(value, path, out, error))
                return false;
            if (out < 0.0f)
            {
                error = std::format("{}: must be >= 0", path);
                return false;
            }
            return true;
        }

        bool ReadVector3(const Json &value, const std::string &path, glm::vec3 &out, std::string &error)
        {
            if (!value.is_array() || value.size() != 3)
            {
                error = std::format("{}: expected 3 numbers", path);
                return false;
            }

            glm::vec3 components{0.0f};
            for (std::size_t i = 0; i < 3; ++i)
            {
                if (!ReadNumber(value[i], std::format("{}[{}]", path, i), components[i], error))
                    return false;
            }

            out = components;
            return true;
        }

        bool ReadRequiredPositive(const Json &object, const char *key, const std::string &path, float &out,
            std::string &error)
        {
            const auto entry = object.find(key);
            if (entry == object.end())
            {
                error = std::format("{}: missing", path);
                return false;
            }
            return ReadPositiveNumber(*entry, path, out, error);
        }

        bool ReadRequiredNumber(const Json &object, const char *key, const std::string &path, float &out,
            std::string &error)
        {
            const auto entry = object.find(key);
            if (entry == object.end())
            {
                error = std::format("{}: missing", path);
                return false;
            }
            return ReadNumber(*entry, path, out, error);
        }

        bool ReadRequiredNonNegative(const Json &object, const char *key, const std::string &path, float &out,
            std::string &error)
        {
            const auto entry = object.find(key);
            if (entry == object.end())
            {
                error = std::format("{}: missing", path);
                return false;
            }
            return ReadNonNegativeNumber(*entry, path, out, error);
        }

        bool ReadRequiredVector3(const Json &object, const char *key, const std::string &path, glm::vec3 &out,
            std::string &error)
        {
            const auto entry = object.find(key);
            if (entry == object.end())
            {
                error = std::format("{}: missing", path);
                return false;
            }
            return ReadVector3(*entry, path, out, error);
        }

        bool ReadUnitInterval(const Json &value, const std::string &path, float &out, std::string &error)
        {
            if (!ReadNumber(value, path, out, error))
                return false;
            if (out < 0.0f || out > 1.0f)
            {
                error = std::format("{}: must be in 0..1", path);
                return false;
            }
            return true;
        }

        bool ReadRequiredUnitInterval(const Json &object, const char *key, const std::string &path, float &out,
            std::string &error)
        {
            const auto entry = object.find(key);
            if (entry == object.end())
            {
                error = std::format("{}: missing", path);
                return false;
            }
            return ReadUnitInterval(*entry, path, out, error);
        }

        bool ParseActuator(const Json &actuators, const char *key, AircraftConfig::Actuator &out, std::string &error)
        {
            const std::string path = std::format("actuators.{}", key);
            const auto entry = actuators.find(key);
            if (entry == actuators.end())
            {
                error = std::format("{}: missing", path);
                return false;
            }
            if (!entry->is_object())
            {
                error = std::format("{}: expected an object", path);
                return false;
            }

            float degrees = 0.0f;
            if (!ReadRequiredPositive(*entry, "maxDeflectionDeg", path + ".maxDeflectionDeg", degrees, error))
                return false;
            out.maxDeflection = glm::radians(degrees);
            if (!ReadRequiredPositive(*entry, "rateDegPerSec", path + ".rateDegPerSec", degrees, error))
                return false;
            out.rate = glm::radians(degrees);

            const auto delay = entry->find("delay");
            if (delay == entry->end())
            {
                error = std::format("{}.delay: missing", path);
                return false;
            }
            return ReadNonNegativeNumber(*delay, path + ".delay", out.delay, error);
        }

        bool ParseFlaps(const Json &actuators, AircraftConfig::Flaps &out, std::string &error)
        {
            const auto entry = actuators.find("flaps");
            if (entry == actuators.end())
            {
                error = "actuators.flaps: missing";
                return false;
            }
            if (!entry->is_object())
            {
                error = "actuators.flaps: expected an object";
                return false;
            }

            const auto detents = entry->find("detentsDeg");
            if (detents == entry->end())
            {
                error = "actuators.flaps.detentsDeg: missing";
                return false;
            }
            if (!detents->is_array())
            {
                error = "actuators.flaps.detentsDeg: expected an array";
                return false;
            }
            if (detents->empty())
            {
                error = "actuators.flaps.detentsDeg: must not be empty";
                return false;
            }

            float previous = 0.0f;
            for (std::size_t i = 0; i < detents->size(); ++i)
            {
                float degrees = 0.0f;
                if (!ReadNonNegativeNumber((*detents)[i], std::format("actuators.flaps.detentsDeg[{}]", i), degrees,
                        error))
                    return false;
                if (i > 0 && degrees <= previous)
                {
                    error = "actuators.flaps.detentsDeg: must be strictly increasing";
                    return false;
                }
                previous = degrees;
                out.detents.push_back(glm::radians(degrees));
            }

            float rate = 0.0f;
            if (!ReadRequiredPositive(*entry, "rateDegPerSec", "actuators.flaps.rateDegPerSec", rate, error))
                return false;
            out.rate = glm::radians(rate);
            return true;
        }

        bool ParseTrim(const Json &actuators, AircraftConfig::Trim &out, std::string &error)
        {
            const auto entry = actuators.find("trim");
            if (entry == actuators.end())
            {
                error = "actuators.trim: missing";
                return false;
            }
            if (!entry->is_object())
            {
                error = "actuators.trim: expected an object";
                return false;
            }

            float step = 0.0f;
            if (!ReadRequiredPositive(*entry, "stepDeg", "actuators.trim.stepDeg", step, error))
                return false;
            out.step = glm::radians(step);

            float max = 0.0f;
            const auto maxEntry = entry->find("maxDeg");
            if (maxEntry == entry->end())
            {
                error = "actuators.trim.maxDeg: missing";
                return false;
            }
            if (!ReadNonNegativeNumber(*maxEntry, "actuators.trim.maxDeg", max, error))
                return false;
            out.max = glm::radians(max);
            return true;
        }

        bool ParseActuators(const Json &document, AircraftConfig::Actuators &out, std::string &error)
        {
            const auto entry = document.find("actuators");
            if (entry == document.end())
            {
                error = "actuators: missing";
                return false;
            }
            if (!entry->is_object())
            {
                error = "actuators: expected an object";
                return false;
            }

            return ParseActuator(*entry, "elevator", out.elevator, error)
                && ParseActuator(*entry, "aileron", out.aileron, error)
                && ParseActuator(*entry, "rudder", out.rudder, error) && ParseFlaps(*entry, out.flaps, error)
                && ParseTrim(*entry, out.trim, error);
        }

        bool ParseEngineCurve(const Json &engine, const char *key, const std::string &path, std::size_t minimumPoints,
            bool unitIntervalX, std::vector<CurvePoint> &out, std::string &error)
        {
            const auto entry = engine.find(key);
            if (entry == engine.end())
            {
                error = std::format("{}: missing", path);
                return false;
            }
            if (!entry->is_array())
            {
                error = std::format("{}: expected an array", path);
                return false;
            }
            if (entry->size() < minimumPoints)
            {
                error = minimumPoints == 1
                    ? std::format("{}: must not be empty", path)
                    : std::format("{}: must have at least {} points", path, minimumPoints);
                return false;
            }

            float previous = 0.0f;
            for (std::size_t i = 0; i < entry->size(); ++i)
            {
                const Json &row = (*entry)[i];
                const std::string rowPath = std::format("{}[{}]", path, i);
                if (!row.is_array() || row.size() != 2)
                {
                    error = std::format("{}: expected [x, y]", rowPath);
                    return false;
                }

                CurvePoint point;
                if (!ReadNumber(row[0], rowPath + "[0]", point.x, error)
                    || !ReadNumber(row[1], rowPath + "[1]", point.y, error))
                    return false;
                if (unitIntervalX && (point.x < 0.0f || point.x > 1.0f))
                {
                    error = std::format("{}[0]: must be in 0..1", rowPath);
                    return false;
                }
                if (point.y < 0.0f)
                {
                    error = std::format("{}[1]: must be >= 0", rowPath);
                    return false;
                }
                if (i > 0 && point.x <= previous)
                {
                    error = std::format("{}: x must be strictly increasing", path);
                    return false;
                }
                previous = point.x;
                out.push_back(point);
            }
            return true;
        }

        bool ParseEngine(const Json &value, const std::string &path, AircraftConfig::Engine &out, std::string &error)
        {
            if (!value.is_object())
            {
                error = std::format("{}: expected an object", path);
                return false;
            }

            if (!ReadRequiredVector3(value, "position", path + ".position", out.position, error))
                return false;
            if (!ReadRequiredVector3(value, "direction", path + ".direction", out.direction, error))
                return false;
            if (glm::length(out.direction) <= 0.0f)
            {
                error = std::format("{}.direction: must be non-zero", path);
                return false;
            }
            out.direction = glm::normalize(out.direction);

            if (!ReadRequiredNonNegative(value, "idleRpm", path + ".idleRpm", out.idleRpm, error))
                return false;
            if (!ReadRequiredNumber(value, "maxRpm", path + ".maxRpm", out.maxRpm, error))
                return false;
            if (out.maxRpm <= out.idleRpm)
            {
                error = std::format("{}.maxRpm: must be > idleRpm", path);
                return false;
            }
            if (!ReadRequiredPositive(value, "rpmTau", path + ".rpmTau", out.rpmTau, error))
                return false;

            return ParseEngineCurve(value, "staticThrust", path + ".staticThrust", 2, true, out.staticThrust, error)
                && ParseEngineCurve(value, "speedFactor", path + ".speedFactor", 1, false, out.speedFactor, error);
        }

        bool ParseEngines(const Json &document, std::vector<AircraftConfig::Engine> &out, std::string &error)
        {
            const auto entry = document.find("engines");
            if (entry == document.end())
            {
                error = "engines: missing";
                return false;
            }
            if (!entry->is_array())
            {
                error = "engines: expected an array";
                return false;
            }
            if (entry->size() > kMaxEngines)
            {
                error = std::format("engines: at most {} engines", kMaxEngines);
                return false;
            }

            for (std::size_t i = 0; i < entry->size(); ++i)
            {
                AircraftConfig::Engine engine;
                if (!ParseEngine((*entry)[i], std::format("engines[{}]", i), engine, error))
                    return false;
                out.push_back(std::move(engine));
            }
            return true;
        }

        bool ParseGear(const Json &value, const std::string &path, AircraftConfig::Gear &out, std::string &error)
        {
            if (!value.is_object())
            {
                error = std::format("{}: expected an object", path);
                return false;
            }

            const auto name = value.find("name");
            if (name == value.end())
            {
                error = std::format("{}.name: missing", path);
                return false;
            }
            if (!name->is_string())
            {
                error = std::format("{}.name: expected a string", path);
                return false;
            }
            out.name = name->get<std::string>();

            float degrees = 0.0f;
            if (!ReadRequiredVector3(value, "mount", path + ".mount", out.mount, error)
                || !ReadRequiredPositive(value, "restLength", path + ".restLength", out.restLength, error)
                || !ReadRequiredPositive(value, "stiffness", path + ".stiffness", out.stiffness, error)
                || !ReadRequiredNonNegative(value, "damping", path + ".damping", out.damping, error)
                || !ReadRequiredNonNegative(value, "rollingFriction", path + ".rollingFriction", out.rollingFriction,
                    error)
                || !ReadRequiredNonNegative(value, "sideFriction", path + ".sideFriction", out.sideFriction, error)
                || !ReadRequiredNonNegative(value, "brake", path + ".brake", out.brake, error)
                || !ReadRequiredNonNegative(value, "maxSteerDeg", path + ".maxSteerDeg", degrees, error))
                return false;

            out.maxSteer = glm::radians(degrees);
            return true;
        }

        bool ParseGears(const Json &document, std::vector<AircraftConfig::Gear> &out, std::string &error)
        {
            const auto entry = document.find("gear");
            if (entry == document.end())
            {
                error = "gear: missing";
                return false;
            }
            if (!entry->is_array())
            {
                error = "gear: expected an array";
                return false;
            }
            if (entry->size() > kMaxGear)
            {
                error = std::format("gear: at most {} gear points", kMaxGear);
                return false;
            }

            for (std::size_t i = 0; i < entry->size(); ++i)
            {
                AircraftConfig::Gear gear;
                if (!ParseGear((*entry)[i], std::format("gear[{}]", i), gear, error))
                    return false;
                out.push_back(std::move(gear));
            }
            return true;
        }

        bool ParseL1Profile(const Json &value, L1Profile &out, std::string &error)
        {
            if (!value.is_object())
            {
                error = "aero.L1: expected an object";
                return false;
            }

            return ReadRequiredNumber(value, "CL", "aero.L1.CL", out.CL, error)
                && ReadRequiredNonNegative(value, "CD", "aero.L1.CD", out.CD, error)
                && ReadRequiredNonNegative(value, "Ka", "aero.L1.Ka", out.Ka, error)
                && ReadRequiredNonNegative(value, "Ke", "aero.L1.Ke", out.Ke, error)
                && ReadRequiredNonNegative(value, "Kr", "aero.L1.Kr", out.Kr, error)
                && ReadRequiredNonNegative(value, "Kp", "aero.L1.Kp", out.Kp, error)
                && ReadRequiredNonNegative(value, "Kq", "aero.L1.Kq", out.Kq, error)
                && ReadRequiredNonNegative(value, "Kn", "aero.L1.Kn", out.Kn, error);
        }

        bool ParseL2PostStall(const Json &stall, L2Stall &out, std::string &error)
        {
            const auto entry = stall.find("postStall");
            if (entry == stall.end())
            {
                error = "aero.L2.stall.postStall: missing";
                return false;
            }
            if (!entry->is_array())
            {
                error = "aero.L2.stall.postStall: expected an array";
                return false;
            }
            if (entry->size() < 2)
            {
                error = "aero.L2.stall.postStall: must have at least 2 rows";
                return false;
            }

            float previous = 0.0f;
            for (std::size_t i = 0; i < entry->size(); ++i)
            {
                const Json &row = (*entry)[i];
                const std::string path = std::format("aero.L2.stall.postStall[{}]", i);
                if (!row.is_array() || row.size() != 4)
                {
                    error = std::format("{}: expected [alphaDeg, CL, CD, Cm]", path);
                    return false;
                }

                L2PostStallPoint point;
                float alphaDegrees = 0.0f;
                if (!ReadNumber(row[0], path + "[0]", alphaDegrees, error)
                    || !ReadNumber(row[1], path + "[1]", point.CL, error)
                    || !ReadNumber(row[2], path + "[2]", point.CD, error)
                    || !ReadNumber(row[3], path + "[3]", point.Cm, error))
                    return false;

                point.alpha = glm::radians(alphaDegrees);
                if (i > 0 && point.alpha <= previous)
                {
                    error = "aero.L2.stall.postStall: must be strictly increasing in alpha";
                    return false;
                }
                previous = point.alpha;
                out.postStall.push_back(point);
            }
            return true;
        }

        bool ParseL2Stall(const Json &value, L2Stall &out, std::string &error)
        {
            const auto entry = value.find("stall");
            if (entry == value.end())
            {
                error = "aero.L2.stall: missing";
                return false;
            }
            if (!entry->is_object())
            {
                error = "aero.L2.stall: expected an object";
                return false;
            }

            float degrees = 0.0f;
            if (!ReadRequiredPositive(*entry, "positiveDeg", "aero.L2.stall.positiveDeg", degrees, error))
                return false;
            out.positive = glm::radians(degrees);

            if (!ReadRequiredNumber(*entry, "negativeDeg", "aero.L2.stall.negativeDeg", degrees, error))
                return false;
            if (degrees >= 0.0f)
            {
                error = "aero.L2.stall.negativeDeg: must be < 0";
                return false;
            }
            out.negative = glm::radians(degrees);

            if (!ReadRequiredPositive(*entry, "blendDeg", "aero.L2.stall.blendDeg", degrees, error))
                return false;
            out.blend = glm::radians(degrees);

            if (!ReadRequiredUnitInterval(*entry, "controlScale", "aero.L2.stall.controlScale",
                    out.controlScale, error)
                || !ReadRequiredUnitInterval(*entry, "stabilityScale", "aero.L2.stall.stabilityScale",
                    out.stabilityScale, error))
                return false;

            if (!ParseL2PostStall(*entry, out, error))
                return false;

            if (out.postStall.front().alpha > out.negative || out.postStall.back().alpha < out.positive + out.blend)
            {
                error = "aero.L2.stall.postStall: must cover negativeDeg to positiveDeg + blendDeg";
                return false;
            }
            return true;
        }

        bool ParseL2Flaps(const Json &value, L2Profile &out, std::string &error)
        {
            const auto entry = value.find("flaps");
            if (entry == value.end())
            {
                error = "aero.L2.flaps: missing";
                return false;
            }
            if (!entry->is_array())
            {
                error = "aero.L2.flaps: expected an array";
                return false;
            }
            if (entry->empty())
            {
                error = "aero.L2.flaps: must not be empty";
                return false;
            }

            float previous = 0.0f;
            for (std::size_t i = 0; i < entry->size(); ++i)
            {
                const Json &row = (*entry)[i];
                const std::string path = std::format("aero.L2.flaps[{}]", i);
                if (!row.is_array() || row.size() != 5)
                {
                    error = std::format("{}: expected [flapDeg, dCL, dCD, dCm, stallShiftDeg]", path);
                    return false;
                }

                float values[5] = {};
                for (std::size_t j = 0; j < 5; ++j)
                {
                    if (!ReadNumber(row[j], std::format("{}[{}]", path, j), values[j], error))
                        return false;
                }
                if (values[0] < 0.0f)
                {
                    error = std::format("{}[0]: must be >= 0", path);
                    return false;
                }
                if (i > 0 && values[0] <= previous)
                {
                    error = "aero.L2.flaps: must be strictly increasing in flapDeg";
                    return false;
                }
                previous = values[0];
                out.flaps.push_back(L2FlapPoint{ glm::radians(values[0]), values[1], values[2], values[3],
                    glm::radians(values[4]) });
            }
            return true;
        }

        bool ParseL2Profile(const Json &value, L2Profile &out, std::string &error)
        {
            if (!value.is_object())
            {
                error = "aero.L2: expected an object";
                return false;
            }

            if (!ReadRequiredNumber(value, "CL0", "aero.L2.CL0", out.CL0, error)
                || !ReadRequiredNumber(value, "CLalpha", "aero.L2.CLalpha", out.CLalpha, error)
                || !ReadRequiredNonNegative(value, "CD0", "aero.L2.CD0", out.CD0, error)
                || !ReadRequiredNonNegative(value, "k", "aero.L2.k", out.k, error)
                || !ReadRequiredNumber(value, "Cm0", "aero.L2.Cm0", out.Cm0, error)
                || !ReadRequiredNumber(value, "Cmalpha", "aero.L2.Cmalpha", out.Cmalpha, error)
                || !ReadRequiredNumber(value, "Cmq", "aero.L2.Cmq", out.Cmq, error)
                || !ReadRequiredNumber(value, "Cmde", "aero.L2.Cmde", out.Cmde, error)
                || !ReadRequiredNumber(value, "CYbeta", "aero.L2.CYbeta", out.CYbeta, error)
                || !ReadRequiredNumber(value, "CYdr", "aero.L2.CYdr", out.CYdr, error)
                || !ReadRequiredNumber(value, "Clbeta", "aero.L2.Clbeta", out.Clbeta, error)
                || !ReadRequiredNumber(value, "Clp", "aero.L2.Clp", out.Clp, error)
                || !ReadRequiredNumber(value, "Clda", "aero.L2.Clda", out.Clda, error)
                || !ReadRequiredNumber(value, "Cnbeta", "aero.L2.Cnbeta", out.Cnbeta, error)
                || !ReadRequiredNumber(value, "Cnr", "aero.L2.Cnr", out.Cnr, error)
                || !ReadRequiredNumber(value, "Cndr", "aero.L2.Cndr", out.Cndr, error)
                || !ReadRequiredNumber(value, "Cnda", "aero.L2.Cnda", out.Cnda, error)
                || !ReadRequiredPositive(value, "minAirspeed", "aero.L2.minAirspeed", out.minAirspeed, error))
                return false;

            return ParseL2Stall(value, out.stall, error) && ParseL2Flaps(value, out, error);
        }

        bool ReadFloatArray(const Json &value, const std::string &path, std::vector<float> &out, std::string &error)
        {
            if (!value.is_array())
            {
                error = std::format("{}: expected an array", path);
                return false;
            }
            if (value.empty())
            {
                error = std::format("{}: must not be empty", path);
                return false;
            }

            out.clear();
            out.reserve(value.size());
            for (std::size_t i = 0; i < value.size(); ++i)
            {
                float number = 0.0f;
                if (!ReadNumber(value[i], std::format("{}[{}]", path, i), number, error))
                    return false;
                out.push_back(number);
            }
            return true;
        }

        bool ReadIncreasingArray(const Json &value, const std::string &path, std::vector<float> &out,
            std::string &error)
        {
            if (!ReadFloatArray(value, path, out, error))
                return false;
            for (std::size_t i = 1; i < out.size(); ++i)
            {
                if (out[i] <= out[i - 1])
                {
                    error = std::format("{}: must be strictly increasing", path);
                    return false;
                }
            }
            return true;
        }

        bool ReadAngleArray(const Json &value, const std::string &path, std::vector<float> &out, std::string &error)
        {
            std::vector<float> degrees;
            if (!ReadIncreasingArray(value, path, degrees, error))
                return false;
            out.clear();
            out.reserve(degrees.size());
            for (const float degree : degrees)
                out.push_back(glm::radians(degree));
            return true;
        }

        bool ReadTableArray(const Json &object, const char *key, const std::string &path, std::size_t expected,
            std::vector<float> &out, std::string &error)
        {
            const std::string keyPath = std::format("{}.{}", path, key);
            const auto entry = object.find(key);
            if (entry == object.end())
            {
                error = std::format("{}: missing", keyPath);
                return false;
            }
            if (!entry->is_array())
            {
                error = std::format("{}: expected an array", keyPath);
                return false;
            }
            if (entry->size() != expected)
            {
                error = std::format("{}: expected {} values, got {}", keyPath, expected, entry->size());
                return false;
            }

            out.clear();
            out.reserve(expected);
            for (std::size_t i = 0; i < expected; ++i)
            {
                float number = 0.0f;
                if (!ReadNumber((*entry)[i], std::format("{}[{}]", keyPath, i), number, error))
                    return false;
                out.push_back(number);
            }
            return true;
        }

        bool ParseL3Table(const Json &object, const std::string &path, std::size_t expected, L3Table &out,
            std::string &error)
        {
            return ReadTableArray(object, "CX", path, expected, out.CX, error)
                && ReadTableArray(object, "CY", path, expected, out.CY, error)
                && ReadTableArray(object, "CZ", path, expected, out.CZ, error)
                && ReadTableArray(object, "Cl", path, expected, out.Cl, error)
                && ReadTableArray(object, "Cm", path, expected, out.Cm, error)
                && ReadTableArray(object, "Cn", path, expected, out.Cn, error);
        }

        bool ParseL3Grid(const Json &value, L3Grid &out, std::string &error)
        {
            if (!value.is_object())
            {
                error = "aero.L3.grid: expected an object";
                return false;
            }

            const auto alpha = value.find("alphaDeg");
            if (alpha == value.end())
            {
                error = "aero.L3.grid.alphaDeg: missing";
                return false;
            }
            if (!ReadAngleArray(*alpha, "aero.L3.grid.alphaDeg", out.alpha, error))
                return false;

            const auto beta = value.find("betaDeg");
            if (beta == value.end())
            {
                error = "aero.L3.grid.betaDeg: missing";
                return false;
            }
            if (!ReadAngleArray(*beta, "aero.L3.grid.betaDeg", out.beta, error))
                return false;

            const auto mach = value.find("mach");
            if (mach == value.end())
            {
                error = "aero.L3.grid.mach: missing";
                return false;
            }
            if (!ReadIncreasingArray(*mach, "aero.L3.grid.mach", out.mach, error))
                return false;

            const auto flaps = value.find("flapsDeg");
            if (flaps == value.end())
            {
                error = "aero.L3.grid.flapsDeg: missing";
                return false;
            }
            return ReadAngleArray(*flaps, "aero.L3.grid.flapsDeg", out.flaps, error);
        }

        bool ParseL3Control(const Json &controls, const char *key, std::size_t nAlpha, L3ControlTable &out,
            std::string &error)
        {
            const std::string path = std::format("aero.L3.controls.{}", key);
            const auto entry = controls.find(key);
            if (entry == controls.end())
            {
                error = std::format("{}: missing", path);
                return false;
            }
            if (!entry->is_object())
            {
                error = std::format("{}: expected an object", path);
                return false;
            }

            const auto deflection = entry->find("deflectionDeg");
            if (deflection == entry->end())
            {
                error = std::format("{}.deflectionDeg: missing", path);
                return false;
            }
            if (!ReadAngleArray(*deflection, path + ".deflectionDeg", out.deflection, error))
                return false;

            return ParseL3Table(*entry, path, nAlpha * out.deflection.size(), out.increments, error);
        }

        bool ParseL3Profile(const Json &value, L3Profile &out, std::string &error)
        {
            if (!value.is_object())
            {
                error = "aero.L3: expected an object";
                return false;
            }

            if (!ReadRequiredPositive(value, "minAirspeed", "aero.L3.minAirspeed", out.minAirspeed, error))
                return false;

            const auto grid = value.find("grid");
            if (grid == value.end())
            {
                error = "aero.L3.grid: missing";
                return false;
            }
            if (!ParseL3Grid(*grid, out.grid, error))
                return false;

            const std::size_t nAlpha = out.grid.alpha.size();
            const std::size_t baseCells =
                nAlpha * out.grid.beta.size() * out.grid.mach.size() * out.grid.flaps.size();

            const auto base = value.find("base");
            if (base == value.end())
            {
                error = "aero.L3.base: missing";
                return false;
            }
            if (!base->is_object())
            {
                error = "aero.L3.base: expected an object";
                return false;
            }
            if (!ParseL3Table(*base, "aero.L3.base", baseCells, out.base, error))
                return false;

            const auto controls = value.find("controls");
            if (controls == value.end())
            {
                error = "aero.L3.controls: missing";
                return false;
            }
            if (!controls->is_object())
            {
                error = "aero.L3.controls: expected an object";
                return false;
            }
            if (!ParseL3Control(*controls, "elevator", nAlpha, out.controls.elevator, error)
                || !ParseL3Control(*controls, "aileron", nAlpha, out.controls.aileron, error)
                || !ParseL3Control(*controls, "rudder", nAlpha, out.controls.rudder, error))
                return false;

            const auto rates = value.find("rates");
            if (rates == value.end())
            {
                error = "aero.L3.rates: missing";
                return false;
            }
            if (!rates->is_object())
            {
                error = "aero.L3.rates: expected an object";
                return false;
            }
            return ReadTableArray(*rates, "Clp", "aero.L3.rates", nAlpha, out.rates.Clp, error)
                && ReadTableArray(*rates, "Cmq", "aero.L3.rates", nAlpha, out.rates.Cmq, error)
                && ReadTableArray(*rates, "Cnr", "aero.L3.rates", nAlpha, out.rates.Cnr, error);
        }

        bool ParseAero(const Json &document, AerodynamicProfile &out, std::string &error)
        {
            const auto entry = document.find("aero");
            if (entry == document.end())
                return true;
            if (!entry->is_object())
            {
                error = "aero: expected an object";
                return false;
            }

            if (const auto l1 = entry->find("L1"); l1 != entry->end())
            {
                L1Profile profile;
                if (!ParseL1Profile(*l1, profile, error))
                    return false;
                out.l1 = profile;
            }

            if (const auto l2 = entry->find("L2"); l2 != entry->end())
            {
                L2Profile profile;
                if (!ParseL2Profile(*l2, profile, error))
                    return false;
                out.l2 = profile;
            }

            if (const auto l3 = entry->find("L3"); l3 != entry->end())
            {
                L3Profile profile;
                if (!ParseL3Profile(*l3, profile, error))
                    return false;
                out.l3 = profile;
            }
            return true;
        }

        bool ParseDocument(const Json &document, AircraftConfig &out, std::string &error)
        {
            AircraftConfig config;

            for (const auto &entry : document.items())
            {
                if (!IsKnownKey(entry.key()))
                {
                    error = std::format("{}: unknown key", entry.key());
                    return false;
                }
            }

            const auto version = document.find("version");
            if (version == document.end())
            {
                error = "version: missing";
                return false;
            }
            if (!version->is_number() || version->get<double>() != 1.0)
            {
                error = "version: must be 1";
                return false;
            }
            config.version = 1;

            const auto mass = document.find("mass");
            if (mass == document.end())
            {
                error = "mass: missing";
                return false;
            }
            if (!ReadPositiveNumber(*mass, "mass", config.mass, error))
                return false;

            const auto inertia = document.find("inertia");
            if (inertia == document.end())
            {
                error = "inertia: missing";
                return false;
            }
            if (!inertia->is_object())
            {
                error = "inertia: expected an object";
                return false;
            }
            if (!ReadRequiredVector3(*inertia, "diagonal", "inertia.diagonal", config.inertiaDiagonal, error))
                return false;
            for (std::size_t i = 0; i < 3; ++i)
            {
                if (config.inertiaDiagonal[i] <= 0.0f)
                {
                    error = std::format("inertia.diagonal[{}]: must be > 0", i);
                    return false;
                }
            }
            if (const auto products = inertia->find("products"); products != inertia->end()
                && !ReadVector3(*products, "inertia.products", config.inertiaProducts, error))
                return false;

            if (!ReadRequiredVector3(document, "centerOfMass", "centerOfMass", config.centerOfMass, error))
                return false;

            const auto reference = document.find("reference");
            if (reference == document.end())
            {
                error = "reference: missing";
                return false;
            }
            if (!reference->is_object())
            {
                error = "reference: expected an object";
                return false;
            }
            if (!ReadRequiredPositive(*reference, "area", "reference.area", config.reference.area, error)
                || !ReadRequiredPositive(*reference, "span", "reference.span", config.reference.span, error)
                || !ReadRequiredPositive(*reference, "chord", "reference.chord", config.reference.chord, error))
                return false;

            const auto camera = document.find("camera");
            if (camera == document.end())
            {
                error = "camera: missing";
                return false;
            }
            if (!camera->is_object())
            {
                error = "camera: expected an object";
                return false;
            }
            if (!ReadRequiredVector3(*camera, "eye", "camera.eye", config.camera.eye, error))
                return false;
            const auto chaseDistance = camera->find("chaseDistance");
            if (chaseDistance == camera->end())
            {
                error = "camera.chaseDistance: missing";
                return false;
            }
            if (!ReadNonNegativeNumber(*chaseDistance, "camera.chaseDistance", config.camera.chaseDistance, error))
                return false;
            const auto chaseHeight = camera->find("chaseHeight");
            if (chaseHeight == camera->end())
            {
                error = "camera.chaseHeight: missing";
                return false;
            }
            if (!ReadNonNegativeNumber(*chaseHeight, "camera.chaseHeight", config.camera.chaseHeight, error))
                return false;

            const auto throttleRate = document.find("throttleRate");
            if (throttleRate == document.end())
            {
                error = "throttleRate: missing";
                return false;
            }
            if (!ReadPositiveNumber(*throttleRate, "throttleRate", config.throttleRate, error))
                return false;

            if (const auto margin = document.find("stallWarningMarginDeg"); margin != document.end())
            {
                float degrees = 0.0f;
                if (!ReadNonNegativeNumber(*margin, "stallWarningMarginDeg", degrees, error))
                    return false;
                config.stallWarningMargin = glm::radians(degrees);
            }

            if (!ParseActuators(document, config.actuators, error))
                return false;

            if (!ParseEngines(document, config.engines, error))
                return false;

            if (!ParseGears(document, config.gear, error))
                return false;

            if (!ParseAero(document, config.aero, error))
                return false;

            const auto nodes = document.find("nodes");
            if (nodes != document.end())
            {
                if (!nodes->is_object())
                {
                    error = "nodes: expected an object";
                    return false;
                }
                for (const auto &entry : nodes->items())
                {
                    const std::string &component = entry.key();
                    if (!entry.value().is_string())
                    {
                        error = std::format("nodes.{}: expected a string", component);
                        return false;
                    }
                    config.nodes.push_back(
                        AircraftConfig::Node{ component, entry.value().get<std::string>() });
                }
            }

            out = std::move(config);
            return true;
        }

        bool DecodeAircraftConfig(const AssetBlobView &root, const AssetDependencies &, AircraftConfig &out,
            std::string &error)
        {
            if (root.header.typeTag != kRawAssetTypeTag)
            {
                error = std::format("expected a raw blob, got type tag {:#010x}", root.header.typeTag);
                return false;
            }
            return ParseAircraftConfig(AsStringView(root), out, error);
        }

        // Every number is a float widened to double; write the shortest text that reads back as the same float.
        void ShortenFloats(Json &value)
        {
            if (value.is_number_float())
            {
                char text[32];
                const auto written = std::to_chars(text, text + sizeof(text), static_cast<float>(value.get<double>()));
                double shortest = 0.0;
                std::from_chars(text, written.ptr, shortest);
                value = shortest;
                return;
            }
            // A primitive iterates as itself.
            if (!value.is_structured())
                return;
            for (Json &child : value)
                ShortenFloats(child);
        }

        Json WriteVector3(const glm::vec3 &value) { return Json::array({ value.x, value.y, value.z }); }

        Json WriteCurve(const std::vector<CurvePoint> &points)
        {
            Json curve = Json::array();
            for (const CurvePoint &point : points)
                curve.push_back(Json::array({ point.x, point.y }));
            return curve;
        }

        Json WriteActuator(const AircraftConfig::Actuator &actuator)
        {
            Json out;
            out["maxDeflectionDeg"] = glm::degrees(actuator.maxDeflection);
            out["rateDegPerSec"] = glm::degrees(actuator.rate);
            out["delay"] = actuator.delay;
            return out;
        }

        Json WriteActuators(const AircraftConfig::Actuators &actuators)
        {
            Json detents = Json::array();
            for (const float detent : actuators.flaps.detents)
                detents.push_back(glm::degrees(detent));

            Json out;
            out["elevator"] = WriteActuator(actuators.elevator);
            out["aileron"] = WriteActuator(actuators.aileron);
            out["rudder"] = WriteActuator(actuators.rudder);
            out["flaps"] = Json{ { "detentsDeg", detents },
                { "rateDegPerSec", glm::degrees(actuators.flaps.rate) } };
            out["trim"] = Json{ { "stepDeg", glm::degrees(actuators.trim.step) },
                { "maxDeg", glm::degrees(actuators.trim.max) } };
            return out;
        }

        Json WriteEngines(const std::vector<AircraftConfig::Engine> &engines)
        {
            Json out = Json::array();
            for (const AircraftConfig::Engine &engine : engines)
            {
                Json entry;
                entry["position"] = WriteVector3(engine.position);
                entry["direction"] = WriteVector3(engine.direction);
                entry["idleRpm"] = engine.idleRpm;
                entry["maxRpm"] = engine.maxRpm;
                entry["rpmTau"] = engine.rpmTau;
                entry["staticThrust"] = WriteCurve(engine.staticThrust);
                entry["speedFactor"] = WriteCurve(engine.speedFactor);
                out.push_back(std::move(entry));
            }
            return out;
        }

        Json WriteGear(const std::vector<AircraftConfig::Gear> &gear)
        {
            Json out = Json::array();
            for (const AircraftConfig::Gear &point : gear)
            {
                Json entry;
                entry["name"] = point.name;
                entry["mount"] = WriteVector3(point.mount);
                entry["restLength"] = point.restLength;
                entry["stiffness"] = point.stiffness;
                entry["damping"] = point.damping;
                entry["rollingFriction"] = point.rollingFriction;
                entry["sideFriction"] = point.sideFriction;
                entry["brake"] = point.brake;
                entry["maxSteerDeg"] = glm::degrees(point.maxSteer);
                out.push_back(std::move(entry));
            }
            return out;
        }

        Json WriteL1(const L1Profile &profile)
        {
            Json out;
            out["CL"] = profile.CL;
            out["CD"] = profile.CD;
            out["Ka"] = profile.Ka;
            out["Ke"] = profile.Ke;
            out["Kr"] = profile.Kr;
            out["Kp"] = profile.Kp;
            out["Kq"] = profile.Kq;
            out["Kn"] = profile.Kn;
            return out;
        }

        Json WriteL2(const L2Profile &profile)
        {
            Json out;
            out["CL0"] = profile.CL0;
            out["CLalpha"] = profile.CLalpha;
            out["CD0"] = profile.CD0;
            out["k"] = profile.k;
            out["Cm0"] = profile.Cm0;
            out["Cmalpha"] = profile.Cmalpha;
            out["Cmq"] = profile.Cmq;
            out["Cmde"] = profile.Cmde;
            out["CYbeta"] = profile.CYbeta;
            out["CYdr"] = profile.CYdr;
            out["Clbeta"] = profile.Clbeta;
            out["Clp"] = profile.Clp;
            out["Clda"] = profile.Clda;
            out["Cnbeta"] = profile.Cnbeta;
            out["Cnr"] = profile.Cnr;
            out["Cndr"] = profile.Cndr;
            out["Cnda"] = profile.Cnda;
            out["minAirspeed"] = profile.minAirspeed;

            Json postStall = Json::array();
            for (const L2PostStallPoint &point : profile.stall.postStall)
                postStall.push_back(
                    Json::array({ glm::degrees(point.alpha), point.CL, point.CD, point.Cm }));

            Json stall;
            stall["positiveDeg"] = glm::degrees(profile.stall.positive);
            stall["negativeDeg"] = glm::degrees(profile.stall.negative);
            stall["blendDeg"] = glm::degrees(profile.stall.blend);
            stall["controlScale"] = profile.stall.controlScale;
            stall["stabilityScale"] = profile.stall.stabilityScale;
            stall["postStall"] = std::move(postStall);
            out["stall"] = std::move(stall);

            Json flaps = Json::array();
            for (const L2FlapPoint &point : profile.flaps)
                flaps.push_back(Json::array(
                    { glm::degrees(point.flap), point.dCL, point.dCD, point.dCm, glm::degrees(point.stallShift) }));
            out["flaps"] = std::move(flaps);
            return out;
        }

        Json WriteFloats(const std::vector<float> &values)
        {
            Json out = Json::array();
            for (const float value : values)
                out.push_back(value);
            return out;
        }

        Json WriteDegrees(const std::vector<float> &radians)
        {
            Json out = Json::array();
            for (const float value : radians)
                out.push_back(glm::degrees(value));
            return out;
        }

        Json WriteL3Table(const L3Table &table)
        {
            Json out;
            out["CX"] = WriteFloats(table.CX);
            out["CY"] = WriteFloats(table.CY);
            out["CZ"] = WriteFloats(table.CZ);
            out["Cl"] = WriteFloats(table.Cl);
            out["Cm"] = WriteFloats(table.Cm);
            out["Cn"] = WriteFloats(table.Cn);
            return out;
        }

        Json WriteL3Control(const L3ControlTable &table)
        {
            Json out = WriteL3Table(table.increments);
            out["deflectionDeg"] = WriteDegrees(table.deflection);
            return out;
        }

        Json WriteL3(const L3Profile &profile)
        {
            Json out;
            out["minAirspeed"] = profile.minAirspeed;
            out["grid"] = Json{ { "alphaDeg", WriteDegrees(profile.grid.alpha) },
                { "betaDeg", WriteDegrees(profile.grid.beta) }, { "mach", WriteFloats(profile.grid.mach) },
                { "flapsDeg", WriteDegrees(profile.grid.flaps) } };
            out["base"] = WriteL3Table(profile.base);
            out["controls"] = Json{ { "elevator", WriteL3Control(profile.controls.elevator) },
                { "aileron", WriteL3Control(profile.controls.aileron) },
                { "rudder", WriteL3Control(profile.controls.rudder) } };
            out["rates"] = Json{ { "Clp", WriteFloats(profile.rates.Clp) },
                { "Cmq", WriteFloats(profile.rates.Cmq) }, { "Cnr", WriteFloats(profile.rates.Cnr) } };
            return out;
        }

        Json WriteAero(const AerodynamicProfile &profile)
        {
            Json out;
            if (profile.l1.has_value())
                out["L1"] = WriteL1(*profile.l1);
            if (profile.l2.has_value())
                out["L2"] = WriteL2(*profile.l2);
            if (profile.l3.has_value())
                out["L3"] = WriteL3(*profile.l3);
            return out;
        }
    }

    bool ParseAircraftConfig(std::string_view json, AircraftConfig &out, std::string &error)
    {
        Json document;
        try
        {
            document = Json::parse(json);
        }
        catch (const std::exception &exception)
        {
            error = exception.what();
            return false;
        }

        if (!document.is_object())
        {
            error = "$: expected an object";
            return false;
        }

        return ParseDocument(document, out, error);
    }

    std::string SerializeAircraftConfig(const AircraftConfig &config)
    {
        Json document;
        document["version"] = config.version;
        document["mass"] = config.mass;
        document["inertia"] = Json{ { "diagonal", WriteVector3(config.inertiaDiagonal) },
            { "products", WriteVector3(config.inertiaProducts) } };
        document["centerOfMass"] = WriteVector3(config.centerOfMass);
        document["reference"] = Json{ { "area", config.reference.area }, { "span", config.reference.span },
            { "chord", config.reference.chord } };
        document["camera"] = Json{ { "eye", WriteVector3(config.camera.eye) },
            { "chaseDistance", config.camera.chaseDistance }, { "chaseHeight", config.camera.chaseHeight } };

        if (!config.nodes.empty())
        {
            Json nodes = Json::object();
            for (const AircraftConfig::Node &node : config.nodes)
                nodes[node.component] = node.model;
            document["nodes"] = std::move(nodes);
        }

        document["throttleRate"] = config.throttleRate;
        document["stallWarningMarginDeg"] = glm::degrees(config.stallWarningMargin);
        document["actuators"] = WriteActuators(config.actuators);
        document["engines"] = WriteEngines(config.engines);
        document["gear"] = WriteGear(config.gear);
        if (config.aero.l1.has_value() || config.aero.l2.has_value() || config.aero.l3.has_value())
            document["aero"] = WriteAero(config.aero);

        ShortenFloats(document);
        return document.dump(2);
    }

    const AssetDecoder<AircraftConfig> &AircraftConfigDecoder()
    {
        static constexpr AssetDecoder<AircraftConfig> kDecoder{&DecodeAircraftConfig, {}, nullptr};
        return kDecoder;
    }
}
