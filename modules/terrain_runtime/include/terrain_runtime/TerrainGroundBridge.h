/**
 * @file TerrainGroundBridge.h
 * @author Jackey Cen
 * @brief Registers the scene's terrain as the World's GroundQuery source.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/Registration.h>
#include <core/ecs/GroundQuery.h>
#include <core/math/WorldCoordinates.h>

#include <cstdint>

namespace mir
{
    class World;

    // Unregisters itself when it dies, so a stale bridge never answers for a newer terrain.
    class TerrainGroundBridge final : public IGroundQuerySource
    {
    public:
        explicit TerrainGroundBridge(World &world);

        GroundQueryResult Sample(WorldPosition position) override;
        GroundQueryCoverage Probe(WorldPosition position) override;
        uint64_t Identity() const override;

        // TerrainWorld was replaced, so a running snap must start again.
        void Invalidate();

    private:
        World &mWorld;
        Registration mBinding;
    };
}
