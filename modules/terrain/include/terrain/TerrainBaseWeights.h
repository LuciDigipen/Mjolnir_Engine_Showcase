/**
 * @file TerrainBaseWeights.h
 * @author Jackey Cen
 * @brief Opens the rule weights cached beside an asset's releases.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TerrainWeights.h>

#include <array>
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
    // <releasesRoot>/<assetHex16>/base-weights holds the cache; current there names one <keyHex16> directory.
    inline constexpr std::string_view kTerrainBaseWeightsDirName = "base-weights";
    inline constexpr std::string_view kTerrainBaseWeightsIndexFileName = "weights.tbwi";
    inline constexpr std::string_view kTerrainBaseWeightsPackFileName = "weights.tbwp";

    // Not a release: the index holds the key, the layer table, the reference layout and one payload per tile.
    inline constexpr std::array<std::byte, 4> kTerrainBaseWeightsMagic{std::byte{'M'}, std::byte{'J'}, std::byte{'T'},
                                                                       std::byte{'W'}};
    inline constexpr uint32_t kTerrainBaseWeightsVersion = 1;
    inline constexpr uint32_t kTerrainBaseWeightsHeaderSize = 128;

    std::filesystem::path TerrainBaseWeightsRoot(const std::filesystem::path &releasesRoot, AssetId asset);
    std::filesystem::path TerrainBaseWeightsDir(const std::filesystem::path &releasesRoot, AssetId asset,
                                                TerrainSourceFingerprint key);
    std::filesystem::path TerrainBaseWeightsPointerPath(const std::filesystem::path &releasesRoot, AssetId asset);

    enum class TerrainBaseWeightsErrorCode : uint8_t
    {
        None,
        MissingPointer, // nothing has written a cache
        MissingTile,    // the cache covers only the recipe's tiles
        Corrupt,        // the pointer, the index or a payload disagrees with itself
        Unsupported,    // a format this reader does not know
        Io,
        InvalidInput,
    };

    struct TerrainBaseWeightsError
    {
        TerrainBaseWeightsErrorCode code = TerrainBaseWeightsErrorCode::None;
        std::string message;
    };

    template <typename T> struct TerrainBaseWeightsResult
    {
        std::optional<T> value;
        TerrainBaseWeightsError error;

        bool Ok() const
        {
            return value.has_value();
        }
        explicit operator bool() const
        {
            return Ok();
        }
    };

    struct TerrainEncodedBaseWeights
    {
        std::vector<std::byte> index;
        std::vector<std::byte> pack;
    };

    // Quantises each tile as the release's weight payloads are and writes one pack of all of them.
    TerrainResult<TerrainEncodedBaseWeights> EncodeTerrainBaseWeights(AssetId asset, TerrainSourceFingerprint key,
                                                                      const TerrainLayout &layout,
                                                                      std::span<const std::string> layers,
                                                                      std::span<const TerrainWeightTile> tiles);

    // Reads only; TerrainTool's sync writes the cache.
    class TerrainBaseWeights
    {
      public:
        [[nodiscard]] static TerrainBaseWeightsResult<TerrainBaseWeights> Open(
            const std::filesystem::path &releasesRoot, AssetId asset);
        // One key directory, without the pointer.
        [[nodiscard]] static TerrainBaseWeightsResult<TerrainBaseWeights> OpenDir(const std::filesystem::path &dir,
                                                                                  AssetId asset);

        TerrainSourceFingerprint Key() const
        {
            return mKey;
        }
        const TerrainLayout &Layout() const
        {
            return mLayout;
        }
        std::span<const std::string> Layers() const
        {
            return mLayers;
        }
        // The rule weights before the Edit Layer weight channels, in the layer table's order.
        TerrainBaseWeightsResult<TerrainWeightTile> ReadTile(TileCoord tile) const;

      private:
        TerrainBaseWeights(TerrainSourceFingerprint key, TerrainLayout layout, std::vector<std::string> layers,
                           std::filesystem::path pack, std::vector<TerrainWeightRecord> tiles);

        const TerrainWeightRecord *FindTile(TileCoord tile) const;

        TerrainSourceFingerprint mKey;
        TerrainLayout mLayout;
        std::vector<std::string> mLayers;
        std::filesystem::path mPack;
        std::vector<TerrainWeightRecord> mTiles;
    };

    std::string_view DescribeTerrainBaseWeightsError(TerrainBaseWeightsErrorCode code);
}
