/**
 * @file TerrainLayerWeightQuery.h
 * @author Jackey Cen
 * @brief Resident CPU material layer weight queries over one immutable terrain release.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/math/WorldCoordinates.h>
#include <terrain/TerrainHeightQuery.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainLayout.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainPlacement.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TileKey.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mir
{
    // One material layer with a nonzero share of a point's weights.
    struct TerrainLayerWeight
    {
        std::string name;
        double weight = 0.0;

        bool operator==(const TerrainLayerWeight &) const = default;
    };

    struct TerrainLayerWeightsRequest
    {
        // A query bound to another release answers WrongRelease.
        TerrainReleaseInfo release;
    };

    enum class TerrainLayerWeightsStatus : uint8_t
    {
        Ready,
        Loading,
        OutsideMap,
        // The release has no weight record for that tile, or no weights at all.
        NoWeights,
        Failed,
    };

    enum class TerrainLayerWeightsFailure : uint8_t
    {
        None,
        // Also when the placement names another asset.
        WrongRelease,
        // A world/local conversion that overflows.
        InvalidInput,
        // See TileError.
        LoadFailed,
    };

    // Layers are sorted by weight descending, then name; weights sum to one over the nonzero layers.
    struct TerrainLayerWeightsResult
    {
        TerrainLayerWeightsStatus status = TerrainLayerWeightsStatus::Failed;
        TerrainLayerWeightsFailure failure = TerrainLayerWeightsFailure::None;
        std::vector<TerrainLayerWeight> layers;
        TerrainReleaseInfo release;
        TileCoord tile;

        bool HasWeights() const
        {
            return status == TerrainLayerWeightsStatus::Ready;
        }
    };

    struct TerrainLayerWeightPayloadRequest
    {
        TerrainReleaseId release;
        TileKey tile;

        bool operator==(const TerrainLayerWeightPayloadRequest &) const = default;
    };

    // Never reads files; not thread-safe.
    class TerrainLayerWeightQuery
    {
      public:
        [[nodiscard]] static TerrainLayerWeightQuery Create(const TerrainReleaseHandle &release);

        const TerrainReleaseInfo &Release() const { return mRelease; }
        const TerrainLayout &Layout() const { return mLayout; }

        TerrainLayerWeightsResult WeightsAt(TerrainLocalPosition local, const TerrainLayerWeightsRequest &request);
        TerrainLayerWeightsResult WeightsAt(WorldPosition world, const TerrainPlacement &placement,
                                            const TerrainLayerWeightsRequest &request);

        // Replaces whatever the slot held.
        TerrainPackError Insert(const TerrainReleaseInfo &from, std::shared_ptr<const TerrainDecodedWeights> weights);

        // Queries needing this tile then fail with LoadFailed instead of waiting.
        TerrainPackError MarkFailed(const TerrainReleaseInfo &from, TileCoord tile, TerrainPackError error);

        // Back to Absent, whatever the slot held; the next query needing it requests it again.
        void Evict(TileCoord tile);

        TerrainTileResidency Residency(TileCoord tile) const;
        // Null unless resident.
        std::shared_ptr<const TerrainDecodedWeights> ResidentTile(TileCoord tile) const;
        // Null unless Failed.
        const TerrainPackError *TileError(TileCoord tile) const;

        // Oldest first, without duplicates.
        std::span<const TerrainLayerWeightPayloadRequest> PayloadRequests() const { return mQueue; }
        std::vector<TerrainLayerWeightPayloadRequest> TakePayloadRequests();

        // Resident tiles answered from since the last take, once each.
        std::vector<TerrainLayerWeightPayloadRequest> TakeUsedTiles();

      private:
        struct Slot
        {
            TerrainTileResidency residency = TerrainTileResidency::Absent;
            std::shared_ptr<const TerrainDecodedWeights> weights;
            TerrainPackError error;
            bool used = false;
        };

        TerrainLayerWeightQuery(const TerrainReleaseInfo &release, TerrainLayout layout,
                                std::unordered_set<TileCoord> weighted);

        TerrainPackError CheckSlot(const TerrainReleaseInfo &from, TileCoord tile) const;
        void Request(TileCoord tile);
        void Unqueue(TileCoord tile);

        TerrainReleaseInfo mRelease;
        TerrainLayout mLayout;
        std::unordered_set<TileCoord> mWeighted;
        std::unordered_map<TileCoord, Slot> mSlots;
        std::vector<TerrainLayerWeightPayloadRequest> mQueue;
        std::vector<TerrainLayerWeightPayloadRequest> mUsed;
    };
}
