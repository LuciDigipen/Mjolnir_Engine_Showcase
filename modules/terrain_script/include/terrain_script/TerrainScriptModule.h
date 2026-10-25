/**
 * @file TerrainScriptModule.h
 * @author sumin.park
 * @brief Adds terrain's Lua bindings to the script host.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/Registration.h>

namespace mir
{
    class ScriptModule;

    // A bridge: terrain never sees Lua, and script never sees terrain. Declared after
    // ScriptModule and terrain's own module
    class TerrainScriptModule
    {
    public:
        explicit TerrainScriptModule(ScriptModule &scripts);

        TerrainScriptModule(const TerrainScriptModule &) = delete;
        TerrainScriptModule &operator=(const TerrainScriptModule &) = delete;

    private:
        Registration mBindings;
    };
}
