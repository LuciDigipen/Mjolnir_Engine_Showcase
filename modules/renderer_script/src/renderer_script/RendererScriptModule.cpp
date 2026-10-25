/**
 * @file RendererScriptModule.cpp
 * @author sumin.park
 * @brief Adds the renderer's Lua bindings to the script host.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer_script/RendererScriptModule.h>

#include <renderer_script/PostProcessingBindings.h>

#include <script/BindingApi.h>
#include <script/ScriptModule.h>

namespace mir
{
    RendererScriptModule::RendererScriptModule(ScriptModule &scripts)
        : mBindings(AddBindings(scripts.Host(), &RegisterPostProcessingBindings))
    {
    }
}
