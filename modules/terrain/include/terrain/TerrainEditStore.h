/**
 * @file TerrainEditStore.h
 * @author Jackey Cen
 * @brief Names, loads and plans the on-disk Edit Layer files of a terrain recipe.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainEditLayer.h>
#include <terrain/TileKey.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    inline constexpr std::string_view kTerrainEditLayersSuffix = ".terrain.layers";
    inline constexpr std::string_view kTerrainEditManifestFileName = "layers.json";
    inline constexpr std::string_view kTerrainEditTileSuffix = ".tedit";

    // The sibling of a recipe file; empty when the name does not end with the recipe suffix.
    std::filesystem::path TerrainEditLayersDir(const std::filesystem::path &recipePath);

    std::string TerrainEditLayerDirName(uint32_t sequence);

    std::string TerrainEditTileFileName(TileCoord tile);

    // Nullopt unless name is exactly the file name of the coordinate it spells.
    std::optional<TileCoord> ParseTerrainEditTileFileName(std::string_view name);

    // A missing directory is an empty stack; every other mismatch fails and names its file.
    TerrainResult<std::vector<TerrainEditLayer>> LoadTerrainEditStack(const std::filesystem::path &layersDir);

    struct TerrainEditStackFile
    {
        // Relative to the layers directory.
        std::filesystem::path path;
        std::vector<std::byte> bytes;
    };

    // Pure: the manifest first, then each layer's tiles in (z, x) order.
    TerrainResult<std::vector<TerrainEditStackFile>> PlanTerrainEditStackWrite(
        std::span<const TerrainEditLayer> layers);
} // namespace mir
