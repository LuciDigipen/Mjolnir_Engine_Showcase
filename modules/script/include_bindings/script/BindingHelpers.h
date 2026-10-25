/**
 * @file BindingHelpers.h
 * @author Jackey Cen
 * @brief Vectors and world positions to and from Lua {x, y, z} tables.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/math/WorldReal.h>
#include <glm/vec3.hpp>
#include <sol/sol.hpp>

#include <optional>

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

    inline std::optional<WorldVec3> WorldVec3FromLua(const sol::object &value)
    {
        if (!value.is<sol::table>())
            return std::nullopt;
        const sol::table t = value.as<sol::table>();
        return TryWorldVec3FromDouble(glm::dvec3(t.get_or("x", 0.0), t.get_or("y", 0.0), t.get_or("z", 0.0)));
    }

    inline sol::table Vec3ToLua(sol::state_view lua, const glm::vec3 &value)
    {
        sol::table out = lua.create_table();
        out["x"] = value.x;
        out["y"] = value.y;
        out["z"] = value.z;
        return out;
    }

    // False for a non-table; a missing component reads as 0.
    inline bool ToVec3(const sol::object &value, glm::vec3 &out)
    {
        if (!value.is<sol::table>())
            return false;

        const sol::table t = value.as<sol::table>();
        out = glm::vec3(t.get_or("x", 0.0f), t.get_or("y", 0.0f), t.get_or("z", 0.0f));
        return true;
    }
}
