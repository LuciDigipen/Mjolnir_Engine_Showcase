/**
 * @file InputScriptModule.h
 * @author sumin.park
 * @brief Adds input's Lua bindings to the script host.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/Registration.h>

namespace mir
{
    class ScriptModule;

    // A bridge: input never sees Lua, and script never sees input. Declared after
    // ScriptModule and input's own module
    class InputScriptModule
    {
    public:
        explicit InputScriptModule(ScriptModule &scripts);

        InputScriptModule(const InputScriptModule &) = delete;
        InputScriptModule &operator=(const InputScriptModule &) = delete;

    private:
        Registration mBindings;
    };
}
