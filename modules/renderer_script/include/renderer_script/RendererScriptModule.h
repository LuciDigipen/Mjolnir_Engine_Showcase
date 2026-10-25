/**
 * @file RendererScriptModule.h
 * @author sumin.park
 * @brief Adds the renderer's Lua bindings to the script host.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/Registration.h>

namespace mir
{
    class ScriptModule;

    // A bridge: the renderer never sees Lua, and script never sees the renderer. Declared after
    // ScriptModule and the renderer's own module
    class RendererScriptModule
    {
    public:
        explicit RendererScriptModule(ScriptModule &scripts);

        RendererScriptModule(const RendererScriptModule &) = delete;
        RendererScriptModule &operator=(const RendererScriptModule &) = delete;

    private:
        Registration mBindings;
    };
}
