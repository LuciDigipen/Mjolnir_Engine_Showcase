/**
 * @file TerrainScriptModule.cpp
 * @author sumin.park
 * @brief Adds terrain's Lua bindings to the script host.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_script/TerrainScriptModule.h>

#include <terrain_script/TerrainBindings.h>

#include <script/BindingApi.h>
#include <script/ScriptModule.h>

namespace mir
{
    TerrainScriptModule::TerrainScriptModule(ScriptModule &scripts)
        : mBindings(AddBindings(scripts.Host(), &RegisterTerrainBindings))
    {
    }
}
