/**
 * @file MeshBindings.h
 * @author Quy Dong Ta
 * @brief Extends the World usertype with world:mesh_bounds.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <sol/forward.hpp>

namespace mir
{
    void RegisterMeshBindings(sol::state &lua);
}
