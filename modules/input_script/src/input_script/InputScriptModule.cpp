/**
 * @file InputScriptModule.cpp
 * @author sumin.park
 * @brief Adds input's Lua bindings to the script host.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <input_script/InputScriptModule.h>

#include <input_script/InputBindings.h>

#include <script/BindingApi.h>
#include <script/ScriptModule.h>

namespace mir
{
    InputScriptModule::InputScriptModule(ScriptModule &scripts)
        : mBindings(AddBindings(scripts.Host(), &RegisterInputBindings))
    {
    }
}
