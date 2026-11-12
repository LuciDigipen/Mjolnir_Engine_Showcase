/**
 * @file TerrainEditLayer.h
 * @author Jackey Cen
 * @brief Sparse per-tile terrain edits, their compositing order and encodings.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainLayout.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TileHeights.h>
#include <terrain/TileKey.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mir
{
    inline constexpr uint32_t kTerrainEditFormatVersion = 1;
    inline constexpr uint32_t kTerrainEditManifestVersion = 1;

    enum class TerrainEditBlend : uint32_t
    {
        Additive = 0,
        AlphaBlend = 1,
    };

    struct TerrainEditLayerInfo
    {
        std::string name;
        int32_t priority = 0;
        uint32_t sequence = 0;
        bool enabled = true;
        TerrainEditBlend heightBlend = TerrainEditBlend::Additive;

        bool operator==(const TerrainEditLayerInfo &) const = default;
    };

    struct TerrainEditTile
    {
        TileCoord tile;
        int32_t intervals = 0;
        std::vector<float> values;
        std::vector<float> alpha;

        bool operator==(const TerrainEditTile &) const = default;
    };

    struct TerrainEditLayer
    {
        TerrainEditLayerInfo info;
        std::unordered_map<TileCoord, TerrainEditTile> tiles;

        bool operator==(const TerrainEditLayer &) const = default;
    };

    struct TerrainEditDecodedTile
    {
        TerrainEditTile tile;
        TerrainEditBlend blend = TerrainEditBlend::Additive;
    };

    namespace terraineditformat
    {
        // Spells EDT1 when stored little-endian.
        inline constexpr uint32_t kMagic = 0x31544445;
        inline constexpr uint32_t kHeightChannel = 0;
        inline constexpr size_t kMagicOffset = 0;
        inline constexpr size_t kVersionOffset = 4;
        inline constexpr size_t kTileXOffset = 8;
        inline constexpr size_t kTileZOffset = 12;
        inline constexpr size_t kIntervalsOffset = 16;
        inline constexpr size_t kChannelCountOffset = 20;
        inline constexpr size_t kHeaderSize = 24;
        inline constexpr size_t kKindOffset = 0;
        inline constexpr size_t kBlendOffset = 4;
        inline constexpr size_t kHasAlphaOffset = 8;
        inline constexpr size_t kChannelSize = 12;
        inline constexpr size_t kTrailerSize = 8;
    } // namespace terraineditformat

    // Enabled layers only, ascending priority then sequence; equal keys keep their stack position.
    std::vector<const TerrainEditLayer *> TerrainEditCompositingOrder(std::span<const TerrainEditLayer> layers);

    TerrainResult<TileHeights> CompositeTerrainEditLayers(const TileHeights &base,
                                                          std::span<const TerrainEditLayer> layers);

    // Writes into every layout tile holding the sample; an absent tile is created zero-filled.
    void SetTerrainEditSample(TerrainEditLayer &layer, const TerrainLayout &layout, GlobalSampleIndex sample,
                              float value, float alpha = 0.0f);

    std::vector<GlobalSampleIndex> ValidateTerrainEditSharedSamples(const TerrainEditLayer &layer,
                                                                    const TerrainLayout &layout);

    TerrainResult<std::vector<std::byte>> EncodeTerrainEditTile(const TerrainEditTile &tile, TerrainEditBlend blend);
    TerrainResult<TerrainEditDecodedTile> DecodeTerrainEditTile(std::span<const std::byte> bytes);

    std::string EncodeTerrainEditManifest(std::span<const TerrainEditLayerInfo> layers);
    TerrainResult<std::vector<TerrainEditLayerInfo>> DecodeTerrainEditManifest(std::string_view text);

    uint64_t TerrainEditStackHash(std::span<const TerrainEditLayer> layers);
} // namespace mir
