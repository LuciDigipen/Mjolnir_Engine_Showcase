/**
 * @file TerrainModule.cpp
 * @author sumin.park
 * @brief Terrain components and the TerrainWorld resource.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain/TerrainModule.h>

#include <terrain/ComponentRegistration.h>
#include <terrain/TerrainScene.h>

#include <core/ecs/World.h>

namespace mir
{
    TerrainModule::TerrainModule(ModuleContext context)
    {
        RegisterTerrainComponents();
        context.world.EmplaceResource<TerrainWorld>();
    }
}
