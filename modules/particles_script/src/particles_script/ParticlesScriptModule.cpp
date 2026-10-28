/**
 * @file ParticlesScriptModule.cpp
 * @author sumin.park
 * @brief Adds particle playback controls to the script host.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "particles_script/ParticlesScriptModule.h"

#include "particles_script/ParticleBindings.h"

#include <script/BindingApi.h>
#include <script/ScriptModule.h>

namespace mir
{
    ParticlesScriptModule::ParticlesScriptModule(ScriptModule &scripts)
        : mBindings(AddBindings(scripts.Host(), &RegisterParticleBindings)) {}
}
