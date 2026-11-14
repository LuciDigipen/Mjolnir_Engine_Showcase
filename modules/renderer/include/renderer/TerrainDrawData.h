/**
 * @file TerrainDrawData.h
 * @author Jackey Cen
 * @brief The four terrain words of a per-draw record and their CPU mirror of the raster weight lookup.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace mir
{
    inline constexpr uint32_t kTerrainDrawWords = 4;
    inline constexpr uint32_t kTerrainDrawSlots = 4;
    // Stored as a word's zero, so a draw without weights packs all four words as zero.
    inline constexpr uint32_t kTerrainWeightImageNone = UINT32_MAX;
    inline constexpr uint8_t kTerrainSlotUnused = 0xFF;
    inline constexpr uint8_t kMaxTerrainDrawMaterialLayer = 3;

    inline constexpr std::array<uint32_t, kTerrainDrawWords> kNoTerrainDrawWords{};
    inline constexpr std::array<uint8_t, kTerrainDrawSlots> kDefaultTerrainSlotMap{0, 0, 0, 0};

    // One drawn tile's terrain data as the shader reads it: word [0] is the weight image row plus one,
    // [1] the sampler row, [2] the material layer of each R..A channel (0xFF unused) and [3] 1 / tile size.
    struct TerrainDrawData
    {
        uint32_t weightImageRow = kTerrainWeightImageNone;
        uint32_t weightSamplerRow = 0;
        std::array<uint8_t, kTerrainDrawSlots> slots{};
        float invFootprint = 0.0f;

        std::array<uint32_t, kTerrainDrawWords> Pack() const
        {
            const uint32_t image = weightImageRow == kTerrainWeightImageNone ? 0 : weightImageRow + 1;
            const uint32_t slotMap = uint32_t(slots[0]) | (uint32_t(slots[1]) << 8) | (uint32_t(slots[2]) << 16) |
                                     (uint32_t(slots[3]) << 24);
            return {image, weightSamplerRow, slotMap, std::bit_cast<uint32_t>(invFootprint)};
        }

        static TerrainDrawData Unpack(const std::array<uint32_t, kTerrainDrawWords> &words)
        {
            TerrainDrawData data;
            data.weightImageRow = words[0] == 0 ? kTerrainWeightImageNone : words[0] - 1;
            data.weightSamplerRow = words[1];
            data.slots = {static_cast<uint8_t>(words[2] & 0xFF), static_cast<uint8_t>((words[2] >> 8) & 0xFF),
                          static_cast<uint8_t>((words[2] >> 16) & 0xFF),
                          static_cast<uint8_t>((words[2] >> 24) & 0xFF)};
            data.invFootprint = std::bit_cast<float>(words[3]);
            return data;
        }
    };

    // A record's unused channels stay unused; a used one names the instance table entry for its release layer.
    inline std::array<uint8_t, kTerrainDrawSlots> MapTerrainSlots(const std::array<uint8_t, kTerrainDrawSlots> &recordSlots,
                                                                  std::span<const uint8_t> instanceTable)
    {
        std::array<uint8_t, kTerrainDrawSlots> mapped{};
        for (size_t slot = 0; slot < kTerrainDrawSlots; ++slot)
        {
            if (recordSlots[slot] == kTerrainSlotUnused)
                mapped[slot] = kTerrainSlotUnused;
            else if (recordSlots[slot] < instanceTable.size())
                mapped[slot] = std::min<uint8_t>(instanceTable[recordSlots[slot]], kMaxTerrainDrawMaterialLayer);
            else
                mapped[slot] = 0;
        }
        return mapped;
    }

    // uv = (local.xz * invFootprint * (side - 1) + 0.5) / side, x to u and z to v.
    inline std::array<float, 2> TerrainWeightUv(float localX, float localZ, float invFootprint, uint32_t side)
    {
        if (side == 0)
            return {0.0f, 0.0f};
        const float span = float(side - 1);
        const float scale = invFootprint * span;
        return {(localX * scale + 0.5f) / float(side), (localZ * scale + 0.5f) / float(side)};
    }

    // Adds each channel to the material layer its slot names and normalises by the sum.
    inline std::array<float, kTerrainDrawSlots> AccumulateTerrainLayerWeights(
        const std::array<uint8_t, kTerrainDrawSlots> &slots, const std::array<float, kTerrainDrawSlots> &channels)
    {
        std::array<float, kTerrainDrawSlots> layers{};
        for (size_t slot = 0; slot < kTerrainDrawSlots; ++slot)
            if (slots[slot] != kTerrainSlotUnused && slots[slot] <= kMaxTerrainDrawMaterialLayer)
                layers[slots[slot]] += channels[slot];
        const float sum = layers[0] + layers[1] + layers[2] + layers[3];
        if (!(sum > 0.0f))
            return {1.0f, 0.0f, 0.0f, 0.0f};
        for (float &weight : layers)
            weight /= sum;
        return layers;
    }
}
