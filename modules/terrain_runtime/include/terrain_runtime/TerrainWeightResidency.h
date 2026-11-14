/**
 * @file TerrainWeightResidency.h
 * @author Jackey Cen
 * @brief Keeps one RGBA8 weight texture per requested terrain tile within a GPU budget.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/ITextureUploader.h>
#include <renderer/TerrainDrawData.h>
#include <terrain/TerrainExecutor.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TerrainStreamingSettings.h>
#include <terrain/TerrainViewSelection.h>
#include <terrain/TileKey.h>

#include <array>
#include <cstdint>
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

    struct TerrainWeightWant
    {
        std::shared_ptr<const TerrainReleaseHandle> release;
        TileCoord tile;
        // Meters from the viewer; nearer tiles get the budget first.
        float distance = 0.0f;
    };

    struct TerrainWeightStats
    {
        uint64_t budgetBytes = 0;
        uint64_t residentBytes = 0;
        size_t reading = 0;
        size_t uploading = 0;
        size_t ready = 0;
        size_t failed = 0;
        uint64_t queued = 0;
        uint64_t retired = 0;
        // Wanted tiles left without a texture because the budget was full.
        uint64_t skippedBudget = 0;
    };

    // Main-thread object. A wanted tile's weight payload is read on the executor, uploaded once the read
    // finishes, and kept until it has gone unrequested for the mesh retain time or its release carries a
    // different payload. Budget pressure retires the farthest tiles first. Draws read the words through Words;
    // a tile whose texture is not ready yet draws without weights.
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

        // Drops what the previous frame wanted; every draw then adds its tile with Request.
        void BeginRequest();
        void Request(const TerrainWeightWant &want);
        // The one install point: finished reads, uploader states, expiration, retirement and reads within the
        // budget. A wanted tile keeps its texture until it has gone unrequested for the mesh retain time.
        void Install(double now);

        // The four words of one draw. A Region carries no weights, and a tile without a ready texture is all
        // zero, so its material sees layer 0 at full weight.
        std::array<uint32_t, kTerrainDrawWords> Words(TerrainUnitKind kind, const TileKey &key,
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
        // Frees room by retiring the farthest entries past an incoming tile's distance.
        void MakeRoom(uint64_t bytes, float distance);
        void StartRead(const TileKey &key, const TerrainWeightWant &want, const TerrainWeightRecord &record,
                       double now);
        void TakeFinished();

        std::shared_ptr<TerrainExecutor> mExecutor;
        ITextureUploader &mTextures;
        std::shared_ptr<Shared> mShared;
        std::unordered_map<TileKey, TerrainWeightWant> mWants;
        std::unordered_map<TileKey, Entry> mEntries;
        uint64_t mBudgetBytes = 0;
        double mRetainSeconds = 0.0;
        uint32_t mSamplerRow = UINT32_MAX;
        uint64_t mNextTicket = 1;
        uint64_t mQueued = 0;
        uint64_t mRetired = 0;
        uint64_t mSkippedBudget = 0;
    };
}
