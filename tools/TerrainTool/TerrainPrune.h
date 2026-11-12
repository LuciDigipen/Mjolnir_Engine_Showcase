/**
 * @file TerrainPrune.h
 * @author Jackey Cen
 * @brief Removes an asset's old releases that no running reader holds.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "TerrainReport.h"

#include <assets/AssetId.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace mir::terrainbake
{
    struct TerrainPruneRequest
    {
        std::string idRoot;
        std::string sourcePath;
        std::filesystem::path gameDir;
        // Zero fails at once if a publisher or another prune holds the asset.
        std::chrono::milliseconds lockWait{0};
    };

    // Deletes every release of the asset but current that no reader holds, and every stale .partial directory.
    // Only a holder of the asset's publish lock may call this. What it cannot remove is reported, never failed.
    void PruneTerrainReleases(const std::filesystem::path &releasesRoot, AssetId asset, TerrainReport &report);

    TerrainReport RunTerrainPrune(const TerrainPruneRequest &request);
}
