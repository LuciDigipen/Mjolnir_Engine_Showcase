/**
 * @file TerrainModule.h
 * @author sumin.park
 * @brief Terrain components and the TerrainWorld resource.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/ModuleContext.h>

namespace mir
{
    // The streaming runtime is terrain_runtime's; this is only what terrain's own data needs
    class TerrainModule
    {
    public:
        explicit TerrainModule(ModuleContext context);

        TerrainModule(const TerrainModule &) = delete;
        TerrainModule &operator=(const TerrainModule &) = delete;
    };
}
