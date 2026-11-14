/**
 * @file TerrainOverlay.h
 * @author Jackey Cen
 * @brief Edited source tiles above a baked release, kept as one immutable snapshot.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetId.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TileHeights.h>
#include <terrain/TileKey.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace mir
{
    // Replaced whole, never mutated, so a reader keeps one revision while a newer one replaces it.
    class TerrainOverlay
    {
      public:
        TerrainOverlay(AssetId asset, uint64_t revision,
                       std::unordered_map<TileCoord, std::shared_ptr<const TileHeights>> tiles,
                       std::unordered_map<TileCoord, std::shared_ptr<const TerrainDecodedWeights>> weights = {});

        AssetId Asset() const { return mAsset; }
        uint64_t Revision() const { return mRevision; }
        const std::unordered_map<TileCoord, std::shared_ptr<const TileHeights>> &Tiles() const { return mTiles; }
        const std::unordered_map<TileCoord, std::shared_ptr<const TerrainDecodedWeights>> &WeightTiles() const
        {
            return mWeights;
        }

        // Null unless this snapshot holds the tile.
        const std::shared_ptr<const TileHeights> *Find(TileCoord tile) const;
        // Null unless this snapshot holds the tile's weights.
        const std::shared_ptr<const TerrainDecodedWeights> *FindWeights(TileCoord tile) const;

      private:
        AssetId mAsset;
        uint64_t mRevision = 0;
        std::unordered_map<TileCoord, std::shared_ptr<const TileHeights>> mTiles;
        std::unordered_map<TileCoord, std::shared_ptr<const TerrainDecodedWeights>> mWeights;
    };

    // reason is one line when the snapshot was refused.
    struct TerrainOverlayApplyResult
    {
        bool applied = false;
        std::string reason;
    };

    // Empty when every tile is a source tile of the release with its intervals, finite samples and weight slots
    // that name the release's layers; otherwise one line naming the offending tile.
    std::optional<std::string> ValidateTerrainOverlay(const TerrainOverlay &overlay,
                                                      const TerrainReleaseHandle &release);
}
