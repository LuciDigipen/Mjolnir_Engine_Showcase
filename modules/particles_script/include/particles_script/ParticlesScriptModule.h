/**
 * @file ParticlesScriptModule.h
 * @author sumin.park
 * @brief Registers the particle playback Lua bridge.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/Registration.h>

namespace mir
{
    class ScriptModule;
    class ParticlesScriptModule
    {
    public:
        explicit ParticlesScriptModule(ScriptModule &scripts);
        ParticlesScriptModule(const ParticlesScriptModule &) = delete;
        ParticlesScriptModule &operator=(const ParticlesScriptModule &) = delete;
    private:
        Registration mBindings;
    };
}
