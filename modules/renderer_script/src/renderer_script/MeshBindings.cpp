/**
 * @file MeshBindings.cpp
 * @author Quy Dong Ta
 * @brief Extends the World usertype with world:mesh_bounds.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer_script/MeshBindings.h>

#include <script/BindingHelpers.h>

#include <core/ecs/World.h>
#include <renderer/components/MeshBounds.h>

#include <sol/sol.hpp>

namespace mir
{
    namespace
    {
        // MeshBounds is derived and never registered, so it is read here rather than through world:get.
        sol::object WorldMeshBounds(World &world, Entity entity, sol::this_state state)
        {
            const MeshBounds *bounds = world.GetComponent<MeshBounds>(entity);
            if (bounds == nullptr)
                return sol::nil; // dead entity, no mesh, or not resolved yet

            sol::state_view lua(state);
            sol::table out = lua.create_table();
            out["min"] = Vec3ToLua(lua, bounds->mLocalMin);
            out["max"] = Vec3ToLua(lua, bounds->mLocalMax);
            return out;
        }
    }

    void RegisterMeshBindings(sol::state &lua)
    {
        sol::table worldType = lua["World"];
        worldType.set_function("mesh_bounds", &WorldMeshBounds);
    }
}
