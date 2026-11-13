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
        void MarkEdited(TileCoord tile);
        void MarkEdited(std::span<const TileCoord> tiles);

        // Null with a reason when a base tile is missing or the stack does not composite.
        std::shared_ptr<const TerrainOverlay> Build(AssetId asset, std::span<const TerrainEditLayer> layers,
                                                    const TerrainBaseTileProvider &base, std::string &reason);

        void Clear();
        uint64_t Revision() const { return mRevision; }
        bool Empty() const { return mEdited.empty(); }
        std::vector<TileCoord> EditedTiles() const;

    private:
        std::set<TileCoord, TerrainTileOrder> mEdited;
        uint64_t mRevision = 0;
    };
} // namespace mir
