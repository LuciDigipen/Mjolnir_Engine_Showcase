/**
 * @file TerrainEditOverlay.cpp
 * @author Jackey Cen
 * @brief Composites the tiles a session edited into one overlay snapshot.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_editor/TerrainEditOverlay.h>

#include <terrain/TerrainEditLayer.h>
#include <terrain/TileHeights.h>

#include <unordered_map>
#include <utility>

namespace mir
{
    void TerrainEditOverlaySession::MarkEdited(TileCoord tile)
    {
        mEdited.insert(tile);
    }

    void TerrainEditOverlaySession::MarkEdited(std::span<const TileCoord> tiles)
    {
        for (const TileCoord &tile : tiles)
            mEdited.insert(tile);
    }

    std::shared_ptr<const TerrainOverlay> TerrainEditOverlaySession::Build(
        AssetId asset, std::span<const TerrainEditLayer> layers, const TerrainBaseTileProvider &base,
        std::string &reason)
    {
        reason.clear();
        if (asset.IsNull())
        {
            reason = "No terrain asset is selected.";
            return nullptr;
        }
        if (mEdited.empty())
        {
            reason = "No tiles have been edited.";
            return nullptr;
        }

        std::unordered_map<TileCoord, std::shared_ptr<const TileHeights>> tiles;
        tiles.reserve(mEdited.size());
        for (const TileCoord &tile : mEdited)
        {
            TerrainBaseHeightsResult<TerrainBaseTile> read = base(tile);
            if (!read || !read.value)
            {
                reason = read.error.message.empty() ? "A base heights tile is missing." : read.error.message;
                return nullptr;
            }

            TerrainResult<TileHeights> composite = CompositeTerrainEditLayers(**read.value, layers);
            if (!composite)
            {
                reason = composite.error.message;
                return nullptr;
            }
            tiles.emplace(tile, std::make_shared<const TileHeights>(std::move(*composite.value)));
        }

        ++mRevision;
        return std::make_shared<const TerrainOverlay>(asset, mRevision, std::move(tiles));
    }

    void TerrainEditOverlaySession::Clear()
    {
        mEdited.clear();
        mRevision = 0;
    }

    std::vector<TileCoord> TerrainEditOverlaySession::EditedTiles() const
    {
        return std::vector<TileCoord>(mEdited.begin(), mEdited.end());
    }
} // namespace mir
