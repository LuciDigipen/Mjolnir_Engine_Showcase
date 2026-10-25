/**
 * @file AudioScriptModule.cpp
 * @author sumin.park
 * @brief Adds audio's Lua bindings to the script host.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <audio_script/AudioScriptModule.h>

#include <audio_script/AudioBindings.h>

#include <script/BindingApi.h>
#include <script/ScriptModule.h>

namespace mir
{
    AudioScriptModule::AudioScriptModule(ScriptModule &scripts)
        : mBindings(AddBindings(scripts.Host(), &RegisterAudioBindings))
    {
    }
}
