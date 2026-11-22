/**
 * @file ActionsScriptModule.h
 * @author Quy Dong Ta
 * @brief Adds the actions library's Lua bindings to the script host.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/Registration.h>

namespace mir
{
    class ScriptModule;

    // A bridge: actions never sees Lua, and script never sees actions. Declared after ScriptModule and
    // ActionsModule
    class ActionsScriptModule
    {
    public:
        explicit ActionsScriptModule(ScriptModule &scripts);

        ActionsScriptModule(const ActionsScriptModule &) = delete;
        ActionsScriptModule &operator=(const ActionsScriptModule &) = delete;

    private:
        Registration mBindings;
    };
}
