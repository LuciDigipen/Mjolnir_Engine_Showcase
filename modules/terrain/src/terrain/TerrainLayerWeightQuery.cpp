/**
 * @file TerrainLayerWeightQuery.cpp
 * @author Jackey Cen
 * @brief Resident CPU material layer weight queries over one immutable terrain release.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "terrain/TerrainLayerWeightQuery.h"

#include "PackInternal.h"

#include <terrain/TerrainOverlay.h>
#include <terrain/TileAddressing.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

namespace mir
{
    namespace
    {
        using Code = TerrainPackErrorCode;
        using detail::Describe;
        using detail::Error;

        TerrainLayerWeightsResult Fail(TerrainLayerWeightsResult result, TerrainLayerWeightsFailure failure)
        {
            result.status = TerrainLayerWeightsStatus::Failed;
            result.failure = failure;
            result.layers.clear();
            return result;
        }

        TerrainLayerWeightsResult Sample(const TerrainDecodedWeights &weights, const TerrainLayout &layout,
                                         const TileAddress &address, TerrainLayerWeightsResult result)
        {
            const int32_t side = weights.intervals + 1;
            const int32_t x0 = address.cell.origin.x - layout.TileFirstSample(address.tile.x);
            const int32_t z0 = address.cell.origin.z - layout.TileFirstSample(address.tile.z);
            const int32_t x1 = std::min(x0 + 1, side - 1);
            const int32_t z1 = std::min(z0 + 1, side - 1);
            const double fx = address.cell.fraction.x;
            const double fz = address.cell.fraction.y;

            const auto texel = [&weights, side](int32_t x, int32_t z, size_t slot)
            {
                const size_t index = size_t(z) * size_t(side) + size_t(x);
                return double(std::to_integer<uint8_t>(weights.bytes[index * kTerrainWeightSlots + slot])) / 255.0;
            };

            size_t used = 0;
            while (used < kTerrainWeightSlots && weights.slots[used] != 0xFF)
                ++used;

            std::array<double, kTerrainWeightSlots> channels{};
            double sum = 0.0;
            for (size_t slot = 0; slot < used; ++slot)
            {
                const double bottom = texel(x0, z0, slot) * (1.0 - fx) + texel(x1, z0, slot) * fx;
                const double top = texel(x0, z1, slot) * (1.0 - fx) + texel(x1, z1, slot) * fx;
                channels[slot] = bottom * (1.0 - fz) + top * fz;
                sum += channels[slot];
            }
            if (!(sum > 0.0))
            {
                result.status = TerrainLayerWeightsStatus::NoWeights;
                return result;
            }

            result.layers.reserve(used);
            for (size_t slot = 0; slot < used; ++slot)
            {
                const double weight = channels[slot] / sum;
                if (weight > 0.0)
                    result.layers.push_back({weights.layers[slot], weight});
            }
            std::sort(result.layers.begin(), result.layers.end(),
                      [](const TerrainLayerWeight &a, const TerrainLayerWeight &b)
                      {
                          if (a.weight != b.weight)
                              return a.weight > b.weight;
                          return a.name < b.name;
                      });
            result.status = TerrainLayerWeightsStatus::Ready;
            return result;
        }
    }

    TerrainLayerWeightQuery::TerrainLayerWeightQuery(const TerrainReleaseInfo &release, TerrainLayout layout,
                                                     std::unordered_set<TileCoord> weighted)
        : mRelease(release), mLayout(std::move(layout)), mWeighted(std::move(weighted))
    {
    }

    TerrainLayerWeightQuery TerrainLayerWeightQuery::Create(const TerrainReleaseHandle &release)
    {
        std::unordered_set<TileCoord> weighted;
        for (const TerrainWeightRecord &record : release.Index().weights)
            if (release.Layout().Contains(record.tile))
                weighted.insert(record.tile);
        return TerrainLayerWeightQuery(release.Info(), release.Layout(), std::move(weighted));
    }

    TerrainLayerWeightsResult TerrainLayerWeightQuery::WeightsAt(TerrainLocalPosition local,
                                                                const TerrainLayerWeightsRequest &request)
    {
        TerrainLayerWeightsResult result;
        result.release = mRelease;
        if (request.release != mRelease)
            return Fail(result, TerrainLayerWeightsFailure::WrongRelease);

        const TileAddress address = AddressLocal(mLayout, local);
        if (!address.IsInside())
        {
            result.status = TerrainLayerWeightsStatus::OutsideMap;
            return result;
        }
        const TileCoord tile = address.tile;
        result.tile = tile;
        if (!mWeighted.contains(tile))
        {
            result.status = TerrainLayerWeightsStatus::NoWeights;
            return result;
        }

        const auto it = mSlots.find(tile);
        if (it == mSlots.end())
        {
            Request(tile);
            result.status = TerrainLayerWeightsStatus::Loading;
            return result;
        }
        if (it->second.residency == TerrainTileResidency::Failed)
            return Fail(result, TerrainLayerWeightsFailure::LoadFailed);
        if (it->second.residency != TerrainTileResidency::Resident)
        {
            result.status = TerrainLayerWeightsStatus::Loading;
            return result;
        }

        if (!it->second.used)
        {
            it->second.used = true;
            mUsed.push_back({mRelease.release, {mRelease.asset, tile}});
        }

        return Sample(*it->second.weights, mLayout, address, result);
    }

    TerrainLayerWeightsResult TerrainLayerWeightQuery::WeightsAt(WorldPosition world, const TerrainPlacement &placement,
                                                                const TerrainLayerWeightsRequest &request)
    {
        TerrainLayerWeightsResult result;
        result.release = mRelease;
        if (placement.asset != mRelease.asset)
            return Fail(result, TerrainLayerWeightsFailure::WrongRelease);
        const std::optional<TerrainLocalPosition> local = TryWorldToTerrainLocal(world, placement.translation);
        if (!local)
            return Fail(result, TerrainLayerWeightsFailure::InvalidInput);
        return WeightsAt(*local, request);
    }

    std::optional<TerrainLayerWeightsResult> TerrainLayerWeightQuery::OverlayWeightsAt(
        const TerrainOverlay &overlay, TerrainLocalPosition local, const TerrainLayerWeightsRequest &request)
    {
        if (request.release != mRelease)
            return std::nullopt;

        const TileAddress address = AddressLocal(mLayout, local);
        if (!address.IsInside())
            return std::nullopt;
        const std::shared_ptr<const TerrainDecodedWeights> *held = overlay.FindWeights(address.tile);
        if (!held || !*held)
            return std::nullopt;

        TerrainLayerWeightsResult result;
        result.release = mRelease;
        result.tile = address.tile;
        return Sample(**held, mLayout, address, result);
    }

    std::optional<TerrainLayerWeightsResult> TerrainLayerWeightQuery::OverlayWeightsAt(
        const TerrainOverlay &overlay, WorldPosition world, const TerrainPlacement &placement,
        const TerrainLayerWeightsRequest &request)
    {
        if (placement.asset != mRelease.asset)
            return std::nullopt;
        const std::optional<TerrainLocalPosition> local = TryWorldToTerrainLocal(world, placement.translation);
        if (!local)
            return std::nullopt;
        return OverlayWeightsAt(overlay, *local, request);
    }

    TerrainPackError TerrainLayerWeightQuery::CheckSlot(const TerrainReleaseInfo &from, TileCoord tile) const
    {
        if (from != mRelease)
            return Error(Code::WrongIdentity, "weight tile {} offered to the query of another release", Describe(tile));
        if (!mWeighted.contains(tile))
            return Error(Code::NotFound, "tile {} has no weights in this release", Describe(tile));
        return {};
    }

    TerrainPackError TerrainLayerWeightQuery::Insert(const TerrainReleaseInfo &from,
                                                     std::shared_ptr<const TerrainDecodedWeights> weights)
    {
        if (!weights)
            return Error(Code::InvalidInput, "no weights");
        const TileCoord tile = weights->tile;
        TerrainPackError error = CheckSlot(from, tile);
        if (error.code != Code::None)
            return error;
        if (weights->intervals != mLayout.IntervalsPerTile())
            return Error(Code::InvalidInput, "weight tile {} has {} intervals, the layout has {}", Describe(tile),
                         weights->intervals, mLayout.IntervalsPerTile());

        Slot &slot = mSlots[tile];
        slot = {TerrainTileResidency::Resident, std::move(weights), {}, false};
        Unqueue(tile);
        return {};
    }

    TerrainPackError TerrainLayerWeightQuery::MarkFailed(const TerrainReleaseInfo &from, TileCoord tile,
                                                         TerrainPackError error)
    {
        TerrainPackError refused = CheckSlot(from, tile);
        if (refused.code != Code::None)
            return refused;
        if (error.code == Code::None)
            error.code = Code::Io;
        mSlots[tile] = {TerrainTileResidency::Failed, nullptr, std::move(error), false};
        Unqueue(tile);
        return {};
    }

    void TerrainLayerWeightQuery::Evict(TileCoord tile)
    {
        mSlots.erase(tile);
        Unqueue(tile);
    }

    TerrainTileResidency TerrainLayerWeightQuery::Residency(TileCoord tile) const
    {
        const auto it = mSlots.find(tile);
        return it == mSlots.end() ? TerrainTileResidency::Absent : it->second.residency;
    }

    std::shared_ptr<const TerrainDecodedWeights> TerrainLayerWeightQuery::ResidentTile(TileCoord tile) const
    {
        const auto it = mSlots.find(tile);
        return it == mSlots.end() ? nullptr : it->second.weights;
    }

    const TerrainPackError *TerrainLayerWeightQuery::TileError(TileCoord tile) const
    {
        const auto it = mSlots.find(tile);
        return it != mSlots.end() && it->second.residency == TerrainTileResidency::Failed ? &it->second.error
                                                                                          : nullptr;
    }

    std::vector<TerrainLayerWeightPayloadRequest> TerrainLayerWeightQuery::TakePayloadRequests()
    {
        return std::exchange(mQueue, {});
    }

    std::vector<TerrainLayerWeightPayloadRequest> TerrainLayerWeightQuery::TakeUsedTiles()
    {
        for (const TerrainLayerWeightPayloadRequest &used : mUsed)
            if (const auto slot = mSlots.find(used.tile.coord); slot != mSlots.end())
                slot->second.used = false;
        return std::exchange(mUsed, {});
    }

    void TerrainLayerWeightQuery::Request(TileCoord tile)
    {
        mSlots[tile].residency = TerrainTileResidency::Requested;
        mQueue.push_back({mRelease.release, {mRelease.asset, tile}});
    }

    void TerrainLayerWeightQuery::Unqueue(TileCoord tile)
    {
        std::erase_if(mQueue, [&](const TerrainLayerWeightPayloadRequest &r) { return r.tile.coord == tile; });
    }
}
