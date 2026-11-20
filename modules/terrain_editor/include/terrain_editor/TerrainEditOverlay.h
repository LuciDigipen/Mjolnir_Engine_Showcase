/**
 * @file TerrainEditOverlay.h
 * @author Jackey Cen
 * @brief Composites the tiles a session edited into one overlay snapshot.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain_editor/TerrainEditBase.h>
#include <terrain_editor/TerrainEditTiles.h>

#include <assets/AssetId.h>
#include <terrain/TerrainEditLayer.h>
#include <terrain/TerrainOverlay.h>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace mir
{
    class TerrainEditOverlaySession
    {
    public:
        // Height edits only; paint strokes mark weight-edited tiles.
        void MarkEdited(TileCoord tile);
        void MarkEdited(std::span<const TileCoord> tiles);
        void MarkWeightEdited(TileCoord tile);
        void MarkWeightEdited(std::span<const TileCoord> tiles);

        // Null with a reason when a base tile is missing or the stack does not composite.
        std::shared_ptr<const TerrainOverlay> Build(AssetId asset, std::span<const TerrainEditLayer> layers,
                                                    const TerrainBaseTileProvider &base, std::string &reason);

        // baseWeightsKey makes unchanged weight tiles keep their payload object across Builds.
        std::shared_ptr<const TerrainOverlay> Build(AssetId asset, std::span<const TerrainEditLayer> layers,
                                                    const TerrainBaseTileProvider &base,
                                                    const TerrainBaseWeightTileProvider &weights,
                                                    std::span<const std::string> baseWeightLayers,
                                                    std::span<const std::string> releaseWeightLayers,
                                                    uint64_t baseWeightsKey, std::string &reason);

        void Clear();
        uint64_t Revision() const { return mRevision; }
        bool Empty() const { return mHeightEdited.empty() && mWeightEdited.empty(); }
        bool HasHeightEdits() const { return !mHeightEdited.empty(); }
        bool HasWeightEdits() const { return !mWeightEdited.empty(); }
        std::vector<TileCoord> EditedTiles() const;
        std::vector<TileCoord> EditedWeightTiles() const;

    private:
        std::set<TileCoord, TerrainTileOrder> mHeightEdited;
        std::set<TileCoord, TerrainTileOrder> mWeightEdited;
        std::map<TileCoord, uint64_t, TerrainTileOrder> mWeightKeys;
        std::map<TileCoord, std::shared_ptr<const TerrainDecodedWeights>, TerrainTileOrder> mWeightPayloads;
        uint64_t mRevision = 0;
    };
} // namespace mir
