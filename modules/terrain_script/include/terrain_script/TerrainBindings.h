/**
 * @file TerrainBindings.h
 * @author Jackey Cen
 * @brief Registers resident terrain height queries for Lua scripts.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#pragma once

#include <sol/forward.hpp>

namespace mir
{
    void RegisterTerrainBindings(sol::state &lua);
}
