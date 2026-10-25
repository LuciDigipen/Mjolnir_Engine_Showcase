/**
 * @file AudioScriptModule.h
 * @author sumin.park
 * @brief Adds audio's Lua bindings to the script host.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/Registration.h>

namespace mir
{
    class ScriptModule;

    // A bridge: audio never sees Lua, and script never sees audio. Declared after
    // ScriptModule and audio's own module
    class AudioScriptModule
    {
    public:
        explicit AudioScriptModule(ScriptModule &scripts);

        AudioScriptModule(const AudioScriptModule &) = delete;
        AudioScriptModule &operator=(const AudioScriptModule &) = delete;

    private:
        Registration mBindings;
    };
}
