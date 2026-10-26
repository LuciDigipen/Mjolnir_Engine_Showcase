/**
 * @file BindingHelpers.h
 * @author Jackey Cen
 * @brief Vectors and world positions to and from Lua {x, y, z} tables.
 *
 * The documented form is a table with numeric x, y and z (and w for a rotation). Anything else, a missing key,
 * a non-number, NaN or infinity, is refused with one error that says what is expected, and never read as zero.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/log/Log.h>
#include <core/math/WorldReal.h>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <sol/sol.hpp>

#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace mir
{
    inline sol::table WorldVec3ToLua(sol::state_view lua, const WorldVec3 &value)
    {
        sol::table out = lua.create_table();
        out["x"] = value.x;
        out["y"] = value.y;
        out["z"] = value.z;
        return out;
    }

    inline sol::table Vec3ToLua(sol::state_view lua, const glm::vec3 &value)
    {
        sol::table out = lua.create_table();
        out["x"] = value.x;
        out["y"] = value.y;
        out["z"] = value.z;
        return out;
    }

    // Reads the numbers named by `keys` from a table into `out`. Empty when it is one, else what is wrong,
    // starting with `argument`: "force.y is NaN" or "force must be a table { x = number, y = number, z = number }".
    inline std::string VectorProblem(
        const sol::object &value, std::string_view argument, std::span<const char *const> keys, double *out)
    {
        std::string form = "{ ";
        for (std::size_t i = 0; i < keys.size(); ++i)
            form += (i ? ", " : "") + std::string(keys[i]) + " = number";
        form += " }";

        if (!value.is<sol::table>())
            return std::string(argument) + " must be a table " + form;

        const sol::table table = value.as<sol::table>();
        for (std::size_t i = 0; i < keys.size(); ++i)
        {
            const sol::object component = table[keys[i]];
            if (component.get_type() != sol::type::number)
                return std::string(argument) + " must be a table " + form + ", " + keys[i] + " is missing or not a number";

            const double number = component.as<double>();
            if (std::isnan(number))
                return std::string(argument) + "." + keys[i] + " is NaN";
            if (std::isinf(number))
                return std::string(argument) + "." + keys[i] + " is infinite";
            out[i] = number;
        }
        return {};
    }

    inline constexpr const char *kVec3Keys[] = { "x", "y", "z" };
    inline constexpr const char *kQuatKeys[] = { "x", "y", "z", "w" };

    // A rotation is { x, y, z, w }, and a missing w is 1, so { x, y, z } alone is the identity turned by nothing.
    // Empty when it is one, else what is wrong, starting with `argument`.
    inline std::string QuatProblem(const sol::object &value, std::string_view argument, double *out)
    {
        if (const std::string problem = VectorProblem(value, argument, std::span<const char *const>(kVec3Keys), out);
            !problem.empty())
            return problem;

        out[3] = 1.0;
        const sol::object w = value.as<sol::table>()["w"];
        if (w.get_type() == sol::type::nil)
            return {};
        if (w.get_type() != sol::type::number)
            return std::string(argument) + ".w must be a number";
        out[3] = w.as<double>();
        if (std::isnan(out[3]))
            return std::string(argument) + ".w is NaN";
        if (std::isinf(out[3]))
            return std::string(argument) + ".w is infinite";
        return {};
    }

    // The documented vector form. False, nothing written, for anything else; does not log.
    inline bool ToVec3(const sol::object &value, glm::vec3 &out)
    {
        double components[3]{};
        if (!VectorProblem(value, "", kVec3Keys, components).empty())
            return false;

        out = glm::vec3(static_cast<float>(components[0]), static_cast<float>(components[1]), static_cast<float>(components[2]));
        return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
    }

    inline std::optional<WorldVec3> WorldVec3FromLua(const sol::object &value)
    {
        double components[3]{};
        if (!VectorProblem(value, "", kVec3Keys, components).empty())
            return std::nullopt;
        return TryWorldVec3FromDouble(glm::dvec3(components[0], components[1], components[2]));
    }

    // The same, but logs one error naming the call and the argument when it is not the form.
    inline bool ReadVec3(const sol::object &value, glm::vec3 &out, std::string_view call, std::string_view argument)
    {
        double components[3]{};
        const std::string problem = VectorProblem(value, argument, kVec3Keys, components);
        if (!problem.empty())
        {
            MIR_LOG_ERROR("script: {}: {}", call, problem);
            return false;
        }

        out = glm::vec3(static_cast<float>(components[0]), static_cast<float>(components[1]), static_cast<float>(components[2]));
        if (!std::isfinite(out.x) || !std::isfinite(out.y) || !std::isfinite(out.z))
        {
            MIR_LOG_ERROR("script: {}: {} is too large to use", call, argument);
            return false;
        }
        return true;
    }

    inline std::optional<WorldVec3> ReadWorldVec3(const sol::object &value, std::string_view call, std::string_view argument)
    {
        double components[3]{};
        const std::string problem = VectorProblem(value, argument, kVec3Keys, components);
        if (!problem.empty())
        {
            MIR_LOG_ERROR("script: {}: {}", call, problem);
            return std::nullopt;
        }

        const std::optional<WorldVec3> position = TryWorldVec3FromDouble(glm::dvec3(components[0], components[1], components[2]));
        if (!position)
            MIR_LOG_ERROR("script: {}: {} is outside the range the world can hold", call, argument);
        return position;
    }

    // A rotation, normalized. False, logged, for anything that is not one or has no length.
    inline bool ReadQuat(const sol::object &value, glm::quat &out, std::string_view call, std::string_view argument)
    {
        double c[4]{};
        if (const std::string problem = QuatProblem(value, argument, c); !problem.empty())
        {
            MIR_LOG_ERROR("script: {}: {}", call, problem);
            return false;
        }

        const glm::quat q(static_cast<float>(c[3]), static_cast<float>(c[0]), static_cast<float>(c[1]), static_cast<float>(c[2]));
        const float length = glm::length(q);
        if (!(length > 0.0f) || !std::isfinite(length))
        {
            MIR_LOG_ERROR("script: {}: {} must not be all zeros", call, argument);
            return false;
        }
        out = q / length;
        return true;
    }

    // A finite number. False, logged, for anything else.
    inline bool ReadNumber(const sol::object &value, double &out, std::string_view call, std::string_view argument)
    {
        if (value.get_type() != sol::type::number)
        {
            MIR_LOG_ERROR("script: {}: {} must be a number", call, argument);
            return false;
        }

        out = value.as<double>();
        if (std::isnan(out) || std::isinf(out))
        {
            MIR_LOG_ERROR("script: {}: {} is {}", call, argument, std::isnan(out) ? "NaN" : "infinite");
            return false;
        }
        return true;
    }
}
