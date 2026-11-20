/**
 * @file ActionsScriptModule.cpp
 * @author Quy Dong Ta
 * @brief Adds the actions library's Lua bindings to the script host.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <actions_script/ActionsScriptModule.h>

#include <actions_script/ActionBindings.h>

#include <script/BindingApi.h>
#include <script/ScriptModule.h>

namespace mir
{
    ActionsScriptModule::ActionsScriptModule(ScriptModule &scripts)
        : mBindings(AddBindings(scripts.Host(), &RegisterActionBindings))
    {
    }
}
