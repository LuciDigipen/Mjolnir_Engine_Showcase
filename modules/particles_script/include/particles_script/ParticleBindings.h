/**
 * @file ParticleBindings.h
 * @author sumin.park
 * @brief World particle playback controls without renderer dependencies.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <sol/forward.hpp>

namespace mir
{
    void RegisterParticleBindings(sol::state &lua);
}
