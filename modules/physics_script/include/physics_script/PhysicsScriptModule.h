/**
 * @file PhysicsScriptModule.h
 * @author sumin.park
 * @brief Adds physics's Lua bindings to the script host.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/Registration.h>

namespace mir
{
    class ScriptModule;

    // A bridge: physics never sees Lua, and script never sees physics. Declared after
    // ScriptModule and physics's own module
    class PhysicsScriptModule
    {
    public:
        explicit PhysicsScriptModule(ScriptModule &scripts);

        PhysicsScriptModule(const PhysicsScriptModule &) = delete;
        PhysicsScriptModule &operator=(const PhysicsScriptModule &) = delete;

    private:
        Registration mBindings;
    };
}
