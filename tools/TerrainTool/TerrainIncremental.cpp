/**
 * @file TerrainIncremental.cpp
 * @author Jackey Cen
 * @brief Compares final source heights with a published release and checks that its index can be reused.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainIncremental.h"

#include "TerrainDerivatives.h"

#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainRegionGrid.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <format>
#include <unordered_set>
#include <utility>

namespace mir::terrainbake
{
    namespace
    {
        bool SameBits(std::span<const float> a, std::span<const float> b)
        {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(),
                              [](float x, float y)
                              {
                                  return std::bit_cast<uint32_t>(x) == std::bit_cast<uint32_t>(y);
                              });
        }

        bool SameLayout(const TerrainLayout &a, const TerrainLayout &b)
        {
            return a.SampleSpacing() == b.SampleSpacing() && a.IntervalsPerTile() == b.IntervalsPerTile() &&
                   a.Bounds() == b.Bounds();
        }
    }

    std::vector<TileCoord> TerrainAffectedTiles(const TerrainReleaseHandle &base,
                                                std::span<const TileHeights> finalHeights, std::size_t &changedTiles)
    {
        std::unordered_set<TileCoord> present;
        for (const TileHeights &tile : finalHeights)
            present.insert(tile.Tile());
        std::unordered_set<TileCoord> affected;
        changedTiles = 0;
        for (const TileHeights &tile : finalHeights)
        {
            const TerrainTileRecord *record = base.Index().Find(tile.Tile());
            bool changed = true;
            if (record)
            {
                const TerrainResult<TileHeights> read = base.ReadTileRecord(*record);
                changed = !read || read.value->IntervalsPerTile() != tile.IntervalsPerTile() ||
                          !SameBits(read.value->Samples(), tile.Samples());
            }
            if (!changed)
                continue;
            ++changedTiles;
            for (int32_t dz = -1; dz <= 1; ++dz)
                for (int32_t dx = -1; dx <= 1; ++dx)
                {
                    const int64_t x = int64_t(tile.Tile().x) + dx, z = int64_t(tile.Tile().z) + dz;
                    if (x < INT32_MIN || x > INT32_MAX || z < INT32_MIN || z > INT32_MAX)
                        continue;
                    const TileCoord neighbour{int32_t(x), int32_t(z)};
                    if (present.contains(neighbour))
                        affected.insert(neighbour);
                }
        }
        std::vector<TileCoord> tiles(affected.begin(), affected.end());
        std::sort(tiles.begin(), tiles.end(),
                  [](TileCoord a, TileCoord b)
                  {
                      return std::pair(a.z, a.x) < std::pair(b.z, b.x);
                  });
        return tiles;
    }

    std::optional<std::string> TerrainBaseMismatch(const TerrainReleaseHandle &base, const TerrainRecipe &recipe,
                                                   std::span<const TileHeights> finalHeights,
                                                   const TerrainRegionSettings &regions)
    {
        const TerrainIndex &index = base.Index();
        if (index.release.format != kTerrainFormatVersion)
            return std::format("the base release is format {}, this tool writes {}", index.release.format.value,
                               kTerrainFormatVersion.value);
        if (index.formatMinor != kTerrainFormatMinor)
            return std::format("the base release is format minor {}, this tool writes {}", index.formatMinor,
                               kTerrainFormatMinor);
        if (index.contentVersion != kTerrainHeightsContentVersion)
            return std::format("the base release holds height content {}, this tool writes {}", index.contentVersion,
                               kTerrainHeightsContentVersion);
        if (!SameLayout(index.layout, recipe.layout))
            return "the base release's layout differs from the recipe";

        const std::vector<TerrainLayout> layers = TerrainDerivativeLayers(recipe, finalHeights);
        if (layers.size() != index.layers.size())
            return "the base release's layer list differs from the recipe";
        for (size_t l = 0; l < layers.size(); ++l)
            if (!SameLayout(layers[l], index.layers[l]))
                return "the base release's layer list differs from the recipe";

        size_t baseSources = 0;
        for (const TerrainTileRecord &record : index.tiles)
            if (record.source)
                ++baseSources;
        if (baseSources != finalHeights.size())
            return "the base release has a different set of source tiles";
        for (const TileHeights &tile : finalHeights)
        {
            const TerrainTileRecord *record = index.Find(tile.Tile());
            const std::optional<double> spacing = recipe.SourceSpacing(tile.Tile());
            if (!record || !spacing || record->sampleSpacing != *spacing || record->layer >= layers.size() ||
                layers[record->layer].SampleSpacing() != *spacing ||
                record->samplesPerSide != tile.SamplesPerSide())
                return "the base release's source tiles or spacings differ from the recipe";
        }

        uint32_t unit = 1;
        for (const TileHeights &tile : finalHeights)
        {
            const std::optional<TerrainLayout> layout = recipe.SourceLayout(tile.Tile());
            if (!layout || layout->IntervalsPerTile() != tile.IntervalsPerTile() ||
                recipe.layout.IntervalsPerTile() % layout->IntervalsPerTile() != 0)
                return "the final sources do not have an assigned layout";
            unit = std::max(unit, uint32_t(recipe.layout.IntervalsPerTile() / layout->IntervalsPerTile()));
        }
        const std::optional<TerrainRegionGrid> grid =
            TerrainRegionGrid::Create(recipe.layout, regions.regionTiles, regions.cells, unit);
        if (!grid)
            return "the recipe's Region grid does not fit its layout";
        if (!index.regionGrid || !index.regionGrid->SameGrid(*grid))
            return "the base release's Region grid differs from the recipe";
        return std::nullopt;
    }
}
