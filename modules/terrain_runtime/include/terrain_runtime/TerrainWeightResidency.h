/**
 * @file TerrainWeightResidency.h
 * @author Jackey Cen
 * @brief Keeps one RGBA8 weight texture per requested terrain unit within a GPU budget.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/ITextureUploader.h>
#include <renderer/TerrainDrawData.h>
#include <terrain/TerrainExecutor.h>
#include <terrain/TerrainOverlay.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TerrainStreamingSettings.h>
#include <terrain/TerrainViewSelection.h>
#include <terrain/TileKey.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>

namespace mir
{
    // One instance's release weight layer index to material layer index. Every entry starts at layer 0, so a
    // release's weights only reach material layer 0 until the layered material fills it in.
    class TerrainSlotTable
    {
      public:
        TerrainSlotTable();

        // Values past the last material layer clamp; entries past the table keep their current value.
        void Set(std::span<const uint8_t> layers);

        // A record's unused channels stay unused; a used one names the table entry for its release layer.
        std::array<uint8_t, kTerrainDrawSlots> Map(const std::array<uint8_t, kTerrainDrawSlots> &recordSlots) const;

      private:
        std::array<uint8_t, kMaxTerrainMaterialLayers> mLayers{};
    };

    // A tile's or a Region's weight image; only a Region uses the level.
    struct TerrainWeightUnit
    {
        AssetId asset;
        TerrainUnitKind kind = TerrainUnitKind::Tile;
        uint32_t level = 0;
        TileCoord coord;

        constexpr bool operator==(const TerrainWeightUnit &) const = default;

        struct Hash
        {
            size_t operator()(const TerrainWeightUnit &unit) const noexcept
            {
                size_t h = std::hash<TileKey>{}(TileKey{unit.asset, unit.coord});
                h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(unit.kind)) + 0x9e3779b97f4a7c15ull + (h << 6) +
                     (h >> 2);
                h ^= std::hash<uint32_t>{}(unit.level) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
                return h;
            }
        };
    };

    struct TerrainWeightWant
    {
        std::shared_ptr<const TerrainReleaseHandle> release;
        TerrainWeightUnit unit;
        // Meters from the viewer; nearer units get the budget first within their kind.
        float distance = 0.0f;
        // The asset's edited weight tiles, when it has a snapshot; a tile they hold is uploaded in place of the
        // release payload.
        std::shared_ptr<const TerrainOverlay> overlay;
    };

    struct TerrainWeightStats
    {
        uint64_t budgetBytes = 0;
        uint64_t residentBytes = 0;
        size_t reading = 0;
        size_t uploading = 0;
        size_t ready = 0;
        size_t failed = 0;
        size_t residentRegions = 0;
        uint64_t queued = 0;
        uint64_t retired = 0;
        // Wanted units left without a texture because the budget was full.
        uint64_t skippedBudget = 0;
    };

    // Main-thread object. A wanted unit's weight payload is read on the executor, uploaded once the read
    // finishes, and kept until it has gone unrequested for the mesh retain time or its release carries a
    // different payload. Budget pressure retires tiles before Regions and, within a kind, the farthest unit
    // first. Draws read the words through Words; a unit whose texture is not ready yet draws without weights.
    class TerrainWeightResidency
    {
      public:
        TerrainWeightResidency(const TerrainStreamingSettings::Gpu &settings, std::shared_ptr<TerrainExecutor> executor,
                               ITextureUploader &textures, uint32_t samplerRow);
        ~TerrainWeightResidency();

        TerrainWeightResidency(const TerrainWeightResidency &) = delete;
        TerrainWeightResidency &operator=(const TerrainWeightResidency &) = delete;

        // The budget applies from the next Install.
        void ApplySettings(const TerrainStreamingSettings::Gpu &gpu);

        // Drops what the previous frame wanted; every draw then adds its unit with Request.
        void BeginRequest();
        void Request(const TerrainWeightWant &want);
        // The one install point: finished reads, uploader states, expiration, retirement and reads within the
        // budget. A wanted unit keeps its texture until it has gone unrequested for the mesh retain time.
        void Install(double now);

        // The four words of one draw. A unit without a ready texture is all zero, so its material sees layer 0
        // at full weight.
        std::array<uint32_t, kTerrainDrawWords> Words(const TerrainWeightUnit &unit,
                                                      const TerrainSlotTable &slotTable) const;
        uint32_t SamplerRow() const
        {
            return mSamplerRow;
        }

        // Every texture goes; the caller's uploader must still be alive.
        void RetireAll();

        TerrainWeightStats Stats() const;

      private:
        struct Entry;
        struct Shared;

        uint64_t HeldBytes() const;
        void Retire(Entry &entry);
        bool MakeRoom(uint64_t bytes, const TerrainWeightUnit *incoming = nullptr, float distance = 0.0f);
        bool QueueUpload(Entry &entry, uint32_t side, std::span<const std::byte> bytes);
        void StartRead(const TerrainWeightWant &want, uint32_t side, uint64_t identity,
                       const std::array<uint8_t, kTerrainDrawSlots> &recordSlots, float invFootprint, double now);
        void StartUpload(const TerrainWeightWant &want, const std::shared_ptr<const TerrainDecodedWeights> &weights,
                         float invFootprint, double now);
        void TakeFinished();

        std::shared_ptr<TerrainExecutor> mExecutor;
        ITextureUploader &mTextures;
        std::shared_ptr<Shared> mShared;
        std::unordered_map<TerrainWeightUnit, TerrainWeightWant, TerrainWeightUnit::Hash> mWants;
        std::unordered_map<TerrainWeightUnit, Entry, TerrainWeightUnit::Hash> mEntries;
        uint64_t mBudgetBytes = 0;
        bool mBudgetReduced = false;
        double mRetainSeconds = 0.0;
        uint32_t mSamplerRow = UINT32_MAX;
        uint64_t mNextTicket = 1;
        uint64_t mQueued = 0;
        uint64_t mRetired = 0;
        uint64_t mSkippedBudget = 0;
    };
}
