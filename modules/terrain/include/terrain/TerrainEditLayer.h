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
#include <terrain/TerrainWeights.h>
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
    inline constexpr uint32_t kTerrainEditWeightFormatVersion = 2;
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

    struct TerrainEditWeight
    {
        std::string layer;
        std::vector<float> values;

        bool operator==(const TerrainEditWeight &) const = default;
    };

    struct TerrainEditTile
    {
        TileCoord tile;
        int32_t intervals = 0;
        // Empty when the tile carries weight channels only.
        std::vector<float> values;
        std::vector<float> alpha;
        // Paint channels, one per material layer name, sorted by name.
        std::vector<TerrainEditWeight> weights;

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
        inline constexpr uint32_t kWeightChannel = 1;
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
        // Version 2 channels add a name-table offset after the version 1 fields.
        inline constexpr size_t kNameOffsetOffset = 12;
        inline constexpr size_t kWeightChannelSize = 16;
        // Each entry is a U32 length followed by that many UTF-8 bytes.
        inline constexpr size_t kNameLengthSize = 4;
        inline constexpr size_t kTrailerSize = 8;
    } // namespace terraineditformat

    // Version 2 is the version 1 header and checksum around 16-byte channel entries, the payloads in
    // channel order, and the weight channels' length-prefixed name table. A tile without weight
    // channels keeps the version 1 layout byte for byte.

    // Enabled layers only, ascending priority then sequence; equal keys keep their stack position.
    std::vector<const TerrainEditLayer *> TerrainEditCompositingOrder(std::span<const TerrainEditLayer> layers);

    TerrainResult<TileHeights> CompositeTerrainEditLayers(const TileHeights &base,
                                                          std::span<const TerrainEditLayer> layers);

    struct TerrainEditWeightComposite
    {
        TerrainWeightTile tile;
        // Paint channel names the base tile has no layer for, sorted and unique.
        std::vector<std::string> skippedLayers;
    };

    TerrainResult<TerrainEditWeightComposite> CompositeTerrainEditWeights(const TerrainWeightTile &base,
                                                                         std::span<const TerrainEditLayer> layers);

    // Writes into every layout tile holding the sample; an absent tile is created zero-filled.
    void SetTerrainEditSample(TerrainEditLayer &layer, const TerrainLayout &layout, GlobalSampleIndex sample,
                              float value, float alpha = 0.0f);

    void SetTerrainEditWeightSample(TerrainEditLayer &layer, const TerrainLayout &layout, GlobalSampleIndex sample,
                                    std::string_view materialLayer, float value);

    std::vector<GlobalSampleIndex> ValidateTerrainEditSharedSamples(const TerrainEditLayer &layer,
                                                                    const TerrainLayout &layout);

    TerrainResult<std::vector<std::byte>> EncodeTerrainEditTile(const TerrainEditTile &tile, TerrainEditBlend blend);
    TerrainResult<TerrainEditDecodedTile> DecodeTerrainEditTile(std::span<const std::byte> bytes);

    std::string EncodeTerrainEditManifest(std::span<const TerrainEditLayerInfo> layers);
    TerrainResult<std::vector<TerrainEditLayerInfo>> DecodeTerrainEditManifest(std::string_view text);

    uint64_t TerrainEditStackHash(std::span<const TerrainEditLayer> layers);
    // Weight channels only; 0 when no enabled layer stores one.
    uint64_t TerrainEditWeightStackHash(std::span<const TerrainEditLayer> layers);
} // namespace mir
