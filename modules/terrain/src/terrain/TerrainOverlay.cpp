/**
 * @file TerrainOverlay.cpp
 * @author Jackey Cen
 * @brief Edited source tiles above a baked release, kept as one immutable snapshot.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "terrain/TerrainOverlay.h"

#include "PackInternal.h"

#include <cmath>
#include <format>
#include <utility>

namespace mir
{
    namespace
    {
        using detail::Describe;
        using detail::Hex;
    }

    TerrainOverlay::TerrainOverlay(AssetId asset, uint64_t revision,
                                   std::unordered_map<TileCoord, std::shared_ptr<const TileHeights>> tiles)
        : mAsset(asset), mRevision(revision), mTiles(std::move(tiles))
    {
    }

    const std::shared_ptr<const TileHeights> *TerrainOverlay::Find(TileCoord tile) const
    {
        const auto it = mTiles.find(tile);
        return it == mTiles.end() ? nullptr : &it->second;
    }

    std::optional<std::string> ValidateTerrainOverlay(const TerrainOverlay &overlay,
                                                      const TerrainReleaseHandle &release)
    {
        if (overlay.Asset() != release.Info().asset)
            return std::format("the overlay of asset {} was offered to the release of asset {}",
                               Hex(overlay.Asset().value), Hex(release.Info().asset.value));

        for (const auto &[coord, heights] : overlay.Tiles())
        {
            if (!heights)
                return std::format("overlay tile {} has no heights", Describe(coord));
            if (heights->Tile() != coord)
                return std::format("overlay tile {} holds the heights of tile {}", Describe(coord),
                                   Describe(heights->Tile()));
            const TerrainTileRecord *source = release.Index().Find(coord);
            if (!source || source->layer >= release.Layers().size())
                return std::format("overlay tile {} is not a source tile of release {}", Describe(coord),
                                   Hex(release.Info().release.value));
            const TerrainLayout &layout = release.Layers()[source->layer];
            if (heights->IntervalsPerTile() != layout.IntervalsPerTile())
                return std::format("overlay tile {} has {} intervals; its source in release {} has {}",
                                   Describe(coord), heights->IntervalsPerTile(), Hex(release.Info().release.value),
                                   layout.IntervalsPerTile());
            for (const float height : heights->Samples())
                if (!std::isfinite(height))
                    return std::format("overlay tile {} has a non-finite sample", Describe(coord));
        }
        return std::nullopt;
    }
}
