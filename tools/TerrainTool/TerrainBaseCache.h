/**
 * @file TerrainBaseCache.h
 * @author Jackey Cen
 * @brief Writes the generator's source heights as a local cache beside an asset's releases.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "TerrainRecipe.h"

#include <assets/AssetId.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainPackWriter.h>
#include <terrain/TileHeights.h>

#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace mir::terrainbake
{
    struct TerrainBaseHeightsEncoding
    {
        TerrainSourceFingerprint key;
        // Empty when encoding failed; error then says why.
        std::optional<TerrainEncodedRelease> encoded;
        std::string error;
    };

    // The recipe's fingerprint with its edit stack cleared, so edits never invalidate the cache.
    TerrainSourceFingerprint BaseHeightsFingerprint(const TerrainRecipe &recipe);

    // A Height-only release of these source tiles; the key is also the release id.
    std::optional<TerrainEncodedRelease> EncodeBaseHeights(const TerrainRecipe &recipe, AssetId asset,
                                                          TerrainSourceFingerprint key,
                                                          std::span<const TileHeights> tiles, std::string &error);

    // True when base/current names key and its index is there.
    bool BaseHeightsCurrent(const std::filesystem::path &releasesRoot, AssetId asset, TerrainSourceFingerprint key);

    // Stages the key's directory, renames it into place, replaces base/current last and removes older keys.
    // The caller holds the asset's publish lock.
    bool WriteBaseHeights(const std::filesystem::path &releasesRoot, AssetId asset,
                          const TerrainBaseHeightsEncoding &base, std::string &error);
}
