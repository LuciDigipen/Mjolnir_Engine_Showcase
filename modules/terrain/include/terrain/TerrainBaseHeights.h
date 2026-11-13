/**
 * @file TerrainBaseHeights.h
 * @author Jackey Cen
 * @brief Opens the generator's cached source heights kept beside an asset's releases.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TileHeights.h>
#include <terrain/TileKey.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace mir
{
    // <releasesRoot>/<assetHex16>/base holds the cache; current there names one <keyHex16> directory.
    inline constexpr std::string_view kTerrainBaseHeightsDirName = "base";

    std::filesystem::path TerrainBaseHeightsRoot(const std::filesystem::path &releasesRoot, AssetId asset);
    std::filesystem::path TerrainBaseHeightsDir(const std::filesystem::path &releasesRoot, AssetId asset,
                                                TerrainSourceFingerprint key);
    std::filesystem::path TerrainBaseHeightsPointerPath(const std::filesystem::path &releasesRoot, AssetId asset);

    enum class TerrainBaseHeightsErrorCode : uint8_t
    {
        None,
        MissingPointer, // nothing has written a cache
        MissingTile,    // the cache covers only the recipe's tiles
        Corrupt,        // the pointer, the index or a payload disagrees with itself
        Unsupported,    // a format this reader does not know
        Io,
        InvalidInput,
    };

    struct TerrainBaseHeightsError
    {
        TerrainBaseHeightsErrorCode code = TerrainBaseHeightsErrorCode::None;
        std::string message;
    };

    template <typename T> struct TerrainBaseHeightsResult
    {
        std::optional<T> value;
        TerrainBaseHeightsError error;

        bool Ok() const
        {
            return value.has_value();
        }
        explicit operator bool() const
        {
            return Ok();
        }
    };

    // Reads only; TerrainTool's sync writes the cache.
    class TerrainBaseHeights
    {
      public:
        [[nodiscard]] static TerrainBaseHeightsResult<TerrainBaseHeights> Open(
            const std::filesystem::path &releasesRoot, AssetId asset);

        TerrainSourceFingerprint Key() const
        {
            return mKey;
        }
        const TerrainReleaseHandle &Release() const
        {
            return mRelease;
        }
        // The source heights before any Edit Layer or seam adjustment.
        TerrainBaseHeightsResult<TileHeights> ReadTile(TileCoord tile) const;

      private:
        TerrainBaseHeights(TerrainSourceFingerprint key, TerrainReleaseHandle release);

        TerrainSourceFingerprint mKey;
        TerrainReleaseHandle mRelease;
    };

    std::string_view DescribeTerrainBaseHeightsError(TerrainBaseHeightsErrorCode code);
}
