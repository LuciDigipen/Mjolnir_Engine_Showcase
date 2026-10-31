/**
 * @file TerrainGroundBridge.cpp
 * @author Jackey Cen
 * @brief Registers the scene's terrain as the World's GroundQuery source.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_runtime/TerrainGroundBridge.h>

#include <core/ecs/World.h>
#include <terrain/TerrainGround.h>
#include <terrain/TerrainScene.h>

namespace mir
{
    namespace
    {
        GroundQuery &GroundQueryOf(World &world)
        {
            if (GroundQuery *query = world.TryResource<GroundQuery>())
                return *query;
            return world.EmplaceResource<GroundQuery>();
        }
    }

    TerrainGroundBridge::TerrainGroundBridge(World &world) : mWorld(world)
    {
        mBinding = GroundQueryOf(world).Bind(*this);
    }

    GroundQueryResult TerrainGroundBridge::Sample(WorldPosition position)
    {
        TerrainWorld *world = mWorld.TryResource<TerrainWorld>();
        if (world == nullptr)
        {
            GroundQueryResult result;
            result.diagnostic = "The scene has no terrain.";
            return result;
        }
        return ProbeTerrainGround(*world, position);
    }

    GroundQueryCoverage TerrainGroundBridge::Probe(WorldPosition position)
    {
        TerrainWorld *world = mWorld.TryResource<TerrainWorld>();
        if (world == nullptr)
        {
            GroundQueryCoverage coverage;
            coverage.mDiagnostic = "The scene has no terrain.";
            return coverage;
        }
        return ProbeTerrainGroundCoverage(*world, position);
    }

    uint64_t TerrainGroundBridge::Identity() const
    {
        const TerrainWorld *world = mWorld.TryResource<TerrainWorld>();
        if (world == nullptr || world->Instances().empty())
            return 0;
        return world->Instances().front().release->Info().release.value;
    }

    void TerrainGroundBridge::Invalidate()
    {
        if (GroundQuery *query = mWorld.TryResource<GroundQuery>())
            query->Invalidate();
    }
}
