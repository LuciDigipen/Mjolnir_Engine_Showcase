/**
 * @file PhysicsScriptModule.cpp
 * @author sumin.park
 * @brief Adds physics's Lua bindings to the script host.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics_script/PhysicsScriptModule.h>

#include <physics_script/PhysicsBindings.h>

#include <script/BindingApi.h>
#include <script/ScriptModule.h>

namespace mir
{
    PhysicsScriptModule::PhysicsScriptModule(ScriptModule &scripts)
        : mBindings(AddBindings(scripts.Host(), &RegisterPhysicsBindings))
    {
    }
}
