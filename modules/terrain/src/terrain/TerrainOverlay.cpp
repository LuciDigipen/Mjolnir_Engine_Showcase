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
#include <cstddef>
#include <format>
#include <utility>

namespace mir
{
    namespace
    {
        using Code = TerrainPackErrorCode;
        using detail::Describe;
        using detail::Hex;
    }

    TerrainOverlay::TerrainOverlay(AssetId asset, uint64_t revision,
                                   std::unordered_map<TileCoord, std::shared_ptr<const TileHeights>> tiles,
                                   std::unordered_map<TileCoord, std::shared_ptr<const TerrainDecodedWeights>> weights)
        : mAsset(asset), mRevision(revision), mTiles(std::move(tiles)), mWeights(std::move(weights))
    {
    }

    const std::shared_ptr<const TileHeights> *TerrainOverlay::Find(TileCoord tile) const
    {
        const auto it = mTiles.find(tile);
        return it == mTiles.end() ? nullptr : &it->second;
    }

    const std::shared_ptr<const TerrainDecodedWeights> *TerrainOverlay::FindWeights(TileCoord tile) const
    {
        const auto it = mWeights.find(tile);
        return it == mWeights.end() ? nullptr : &it->second;
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

        const TerrainIndex &index = release.Index();
        for (const auto &[coord, weights] : overlay.WeightTiles())
        {
            if (!weights)
                return std::format("overlay weight tile {} has no weights", Describe(coord));
            if (weights->tile != coord)
                return std::format("overlay weight tile {} holds the weights of tile {}", Describe(coord),
                                   Describe(weights->tile));
            const TerrainTileRecord *source = index.Find(coord);
            if (!source || source->layer >= release.Layers().size())
                return std::format("overlay weight tile {} is not a source tile of release {}", Describe(coord),
                                   Hex(release.Info().release.value));
            if (index.weightLayers.empty())
                return std::format("overlay weight tile {} needs a release with weight layers; release {} has none",
                                   Describe(coord), Hex(release.Info().release.value));
            if (weights->intervals != release.Layout().IntervalsPerTile())
                return std::format("overlay weight tile {} has {} intervals; release {} has {}", Describe(coord),
                                   weights->intervals, Hex(release.Info().release.value),
                                   release.Layout().IntervalsPerTile());
            size_t used = 0;
            const std::string what = "overlay weight tile " + Describe(coord);
            if (const TerrainPackError slots =
                    detail::CheckWeightSlots(weights->slots, index.weightLayers.size(), what, used);
                slots.code != Code::None)
                return slots.message;
            for (size_t slot = 0; slot < used; ++slot)
                if (weights->layers[slot] != index.weightLayers[weights->slots[slot]])
                    return std::format("overlay weight tile {} names '{}' for slot {}; release {} calls it '{}'",
                                       Describe(coord), weights->layers[slot], weights->slots[slot],
                                       Hex(release.Info().release.value),
                                       index.weightLayers[weights->slots[slot]]);
            if (const TerrainPackError texels =
                    detail::CheckWeightTexels(size_t(weights->intervals) + 1, used, weights->bytes, what);
                texels.code != Code::None)
                return texels.message;
        }
        return std::nullopt;
    }
}
