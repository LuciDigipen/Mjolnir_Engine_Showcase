/**
 * @file TerrainLayerWeightQuery.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainLayerWeightQuery.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"

#include <terrain/TerrainLayerWeightQuery.h>
#include <terrain/TileAddressing.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    using Code = TerrainPackErrorCode;
    using Status = TerrainLayerWeightsStatus;
    using Failure = TerrainLayerWeightsFailure;

    TerrainLocalPosition Local(double x, double z)
    {
        return *TerrainLocalPosition::FromMeters({x, 0.0, z});
    }

    TerrainLayout BlockLayout()
    {
        const auto layout = TerrainLayout::CreateFromTileSize(64.0, 4.0, {-128.0, -128.0, 128.0, 128.0});
        REQUIRE(layout);
        return *layout;
    }

    std::vector<TerrainWeightTile> BuildWeights(const TerrainLayout &layout, const std::vector<std::string> &layers)
    {
        std::vector<TerrainWeightTile> tiles;
        const TileRange range = layout.Tiles();
        const int32_t n = layout.IntervalsPerTile();
        for (int32_t z = range.min.z; z <= range.max.z; ++z)
            for (int32_t x = range.min.x; x <= range.max.x; ++x)
            {
                TerrainWeightTile tile;
                tile.tile = {x, z};
                tile.intervals = n;
                tile.layers = layers;
                const size_t count = tile.SampleCount();
                tile.weights.resize(count * layers.size());
                for (int32_t k = 0; k <= n; ++k)
                    for (int32_t i = 0; i <= n; ++i)
                    {
                        const int64_t gi = layout.TileFirstSample(x) + i;
                        const int64_t gk = layout.TileFirstSample(z) + k;
                        const int64_t iu = ((gi * 13 + gk * 7) % 11 + 11) % 11;
                        const int64_t iv = ((gi * 5 + gk * 17) % 11 + 11) % 11;
                        const float u = float(iu) / 10.0f;
                        const float v = float(iv) / 10.0f;
                        const std::array<float, 4> raw{0.05f + 0.2f * u, 0.05f + 0.2f * v,
                                                       0.05f + 0.2f * (1.0f - u), 0.05f + 0.2f * (1.0f - v)};
                        const float sum = raw[0] + raw[1] + raw[2] + raw[3];
                        const size_t sample = size_t(k) * size_t(n + 1) + size_t(i);
                        for (size_t l = 0; l < layers.size(); ++l)
                            tile.weights[l * count + sample] = raw[l] / sum;
                    }
                tiles.push_back(std::move(tile));
            }
        return tiles;
    }

    std::vector<TerrainLayerWeight> Expected(const TerrainDecodedWeights &weights, const TerrainLayout &layout,
                                             TileCoord tile, double x, double z)
    {
        const TileAddress address = AddressLocal(layout, Local(x, z));
        REQUIRE(address.IsInside());
        REQUIRE(address.tile == tile);
        const int32_t side = weights.intervals + 1;
        const int32_t x0 = std::clamp(int32_t(address.cell.origin.x - layout.TileFirstSample(tile.x)), 0, side - 1);
        const int32_t z0 = std::clamp(int32_t(address.cell.origin.z - layout.TileFirstSample(tile.z)), 0, side - 1);
        const int32_t x1 = std::min(x0 + 1, side - 1);
        const int32_t z1 = std::min(z0 + 1, side - 1);
        const double fx = address.cell.fraction.x, fz = address.cell.fraction.y;
        const auto channel = [&weights, side](int32_t ix, int32_t iz, size_t slot)
        {
            const size_t texel = size_t(iz) * size_t(side) + size_t(ix);
            return double(std::to_integer<uint8_t>(weights.bytes[texel * kTerrainWeightSlots + slot])) / 255.0;
        };

        size_t used = 0;
        while (used < kTerrainWeightSlots && weights.slots[used] != 0xFF)
            ++used;
        std::vector<TerrainLayerWeight> expected;
        std::array<double, kTerrainWeightSlots> channels{};
        double sum = 0.0;
        for (size_t slot = 0; slot < used; ++slot)
        {
            const double bottom = channel(x0, z0, slot) * (1.0 - fx) + channel(x1, z0, slot) * fx;
            const double top = channel(x0, z1, slot) * (1.0 - fx) + channel(x1, z1, slot) * fx;
            channels[slot] = bottom * (1.0 - fz) + top * fz;
            sum += channels[slot];
        }
        REQUIRE(sum > 0.0);
        for (size_t slot = 0; slot < used; ++slot)
            if (channels[slot] > 0.0)
                expected.push_back({weights.layers[slot], channels[slot] / sum});
        std::sort(expected.begin(), expected.end(),
                  [](const TerrainLayerWeight &a, const TerrainLayerWeight &b)
                  {
                      if (a.weight != b.weight)
                          return a.weight > b.weight;
                      return a.name < b.name;
                  });
        return expected;
    }

    void CheckLayers(const std::vector<TerrainLayerWeight> &actual, const std::vector<TerrainLayerWeight> &expected)
    {
        REQUIRE(actual.size() == expected.size());
        for (size_t i = 0; i < actual.size(); ++i)
        {
            INFO("layer " << i);
            CHECK(actual[i].name == expected[i].name);
            CHECK(std::abs(actual[i].weight - expected[i].weight) <= 1e-12);
        }
        double sum = 0.0;
        for (const TerrainLayerWeight &layer : actual)
            sum += layer.weight;
        CHECK(std::abs(sum - 1.0) <= 1e-12);
    }

    struct Weighted
    {
        TerrainLayout layout = BlockLayout();
        std::vector<TileHeights> heights = BuildTiles(layout);
        std::vector<std::string> table{"dirt", "sand", "rock", "snow"};
        std::vector<TerrainWeightTile> weights = BuildWeights(layout, table);
        TerrainReleaseInfo info = Info(TerrainAsset("terrain/weight-query.terrain.json"), 0x1A2B);
        TerrainEncodedRelease encoded;
        std::optional<OpenedRelease> opened;
        std::optional<TerrainLayerWeightQuery> query;

        Weighted()
        {
            TerrainWeightsWrites writes;
            writes.layers = table;
            writes.fingerprint = 0x51D3;
            writes.tiles = weights;
            encoded = EncodeWithWeights(info, layout, heights, writes, {.chunkTiles = 2});
            opened.emplace(OpenInMemory(encoded));
            query.emplace(TerrainLayerWeightQuery::Create(opened->handle));
        }

        std::shared_ptr<const TerrainDecodedWeights> Load(TileCoord tile)
        {
            TerrainResult<TerrainDecodedWeights> read = opened->handle.ReadWeights(tile);
            INFO(read.error.message);
            REQUIRE(read);
            return std::make_shared<const TerrainDecodedWeights>(std::move(*read.value));
        }
    };

    struct Plain
    {
        TerrainLayout layout = BlockLayout();
        std::vector<TileHeights> heights = BuildTiles(layout);
        TerrainReleaseInfo info = Info(TerrainAsset("terrain/plain-weights.terrain.json"), 0x0BAD);
        OpenedRelease opened = OpenInMemory(Encode(info, layout, heights, {.chunkTiles = 2}));
        TerrainLayerWeightQuery query = TerrainLayerWeightQuery::Create(opened.handle);
    };
}

TEST_CASE("A weighted release answers loading once, then ready from its resident payload",
          "[terrain][query][weights]")
{
    Weighted w;
    const TerrainLayerWeightsRequest request{w.info};
    const TerrainLocalPosition point = Local(-58.25, 37.5);
    const TileAddress address = AddressLocal(w.layout, point);
    REQUIRE(address.IsInside());
    const TileCoord tile = address.tile;

    const TerrainByteStats before = w.opened->source->Stats();
    const TerrainLayerWeightsResult first = w.query->WeightsAt(point, request);
    CHECK(first.status == Status::Loading);
    CHECK(first.failure == Failure::None);
    CHECK(first.tile == tile);
    CHECK(first.release == w.info);
    CHECK(first.layers.empty());
    REQUIRE(w.query->PayloadRequests().size() == 1);
    CHECK(w.query->PayloadRequests()[0].tile == TileKey{w.info.asset, tile});
    CHECK(w.query->Residency(tile) == TerrainTileResidency::Requested);
    CHECK(w.opened->source->Stats().reads == before.reads);

    for (int probe = 0; probe < 8; ++probe)
        CHECK(w.query->WeightsAt(point, request).status == Status::Loading);
    CHECK(w.query->PayloadRequests().size() == 1);
    CHECK(w.query->WeightsAt(Local(-58.0, 37.0), request).status == Status::Loading);
    CHECK(w.query->PayloadRequests().size() == 1);

    const std::shared_ptr<const TerrainDecodedWeights> payload = w.Load(tile);
    REQUIRE(w.query->Insert(w.info, payload).code == Code::None);
    CHECK(w.query->Residency(tile) == TerrainTileResidency::Resident);
    CHECK(w.query->ResidentTile(tile) == payload);
    CHECK(w.query->PayloadRequests().empty());

    const TerrainLayerWeightsResult ready = w.query->WeightsAt(point, request);
    CHECK(ready.status == Status::Ready);
    CHECK(ready.failure == Failure::None);
    CHECK(ready.tile == tile);
    CHECK(ready.release == w.info);
    CheckLayers(ready.layers, Expected(*payload, w.layout, tile, -58.25, 37.5));
    CHECK(w.opened->source->Stats().reads == before.reads + 1);
}

TEST_CASE("A texel centre answers that texel and a point between texels the bilinear blend",
          "[terrain][query][weights]")
{
    Weighted w;
    const TerrainLayerWeightsRequest request{w.info};
    const TileCoord tile{0, 0};
    const std::shared_ptr<const TerrainDecodedWeights> payload = w.Load(tile);
    REQUIRE(w.query->Insert(w.info, payload).code == Code::None);

    const double tx = w.layout.SamplePosition(w.layout.TileFirstSample(tile.x) + 5);
    const double tz = w.layout.SamplePosition(w.layout.TileFirstSample(tile.z) + 7);
    const TerrainLayerWeightsResult centre = w.query->WeightsAt(Local(tx, tz), request);
    CHECK(centre.status == Status::Ready);
    CheckLayers(centre.layers, Expected(*payload, w.layout, tile, tx, tz));
    for (const TerrainLayerWeight &layer : centre.layers)
    {
        size_t slot = 0;
        while (slot < kTerrainWeightSlots && payload->slots[slot] != 0xFF && payload->layers[slot] != layer.name)
            ++slot;
        REQUIRE(slot < kTerrainWeightSlots);
        REQUIRE(payload->slots[slot] != 0xFF);
        const size_t texel = size_t(7) * size_t(payload->intervals + 1) + size_t(5);
        const double texelWeight =
            double(std::to_integer<uint8_t>(payload->bytes[texel * kTerrainWeightSlots + slot])) / 255.0;
        CHECK(std::abs(layer.weight - texelWeight) <= 1e-12);
    }

    const double spacing = w.layout.SampleSpacing();
    const double bx = tx + 0.25 * spacing;
    const double bz = tz + 0.75 * spacing;
    const TerrainLayerWeightsResult blend = w.query->WeightsAt(Local(bx, bz), request);
    CHECK(blend.status == Status::Ready);
    CheckLayers(blend.layers, Expected(*payload, w.layout, tile, bx, bz));
}

TEST_CASE("A release without weights answers NoWeights without reading", "[terrain][query][weights]")
{
    Plain p;
    const TerrainLayerWeightsRequest request{p.info};
    const auto point = Local(10.0, 10.0);
    const auto address = AddressLocal(p.layout, point);
    REQUIRE(address.IsInside());
    const TerrainByteStats before = p.opened.source->Stats();

    const TerrainLayerWeightsResult result = p.query.WeightsAt(point, request);
    CHECK(result.status == Status::NoWeights);
    CHECK(result.failure == Failure::None);
    CHECK(result.tile == address.tile);
    CHECK(result.layers.empty());
    CHECK(p.query.PayloadRequests().empty());
    CHECK(p.opened.source->Stats().reads == before.reads);
    CHECK(p.opened.source->Stats().bytes == before.bytes);
}

TEST_CASE("A weight query reports OutsideMap, WrongRelease and a failed read", "[terrain][query][weights]")
{
    Weighted w;
    const TerrainLayerWeightsRequest request{w.info};
    const TerrainLocalPosition point = Local(-58.25, 37.5);
    const TileCoord tile = AddressLocal(w.layout, point).tile;

    const TerrainLayerWeightsResult outside = w.query->WeightsAt(Local(1000.0, 0.0), request);
    CHECK(outside.status == Status::OutsideMap);
    CHECK(outside.failure == Failure::None);
    CHECK(outside.layers.empty());
    CHECK(w.query->PayloadRequests().empty());

    TerrainReleaseInfo other = w.info;
    other.release = TerrainReleaseId(0x2B3C);
    const TerrainLayerWeightsResult wrong = w.query->WeightsAt(point, TerrainLayerWeightsRequest{other});
    CHECK(wrong.status == Status::Failed);
    CHECK(wrong.failure == Failure::WrongRelease);
    CHECK(wrong.release == w.info);
    CHECK(wrong.layers.empty());
    CHECK(w.query->PayloadRequests().empty());

    const TerrainLayerWeightsResult loading = w.query->WeightsAt(point, request);
    CHECK(loading.status == Status::Loading);
    REQUIRE(w.query->PayloadRequests().size() == 1);
    REQUIRE(w.query->MarkFailed(w.info, tile, {Code::Io, "controlled failure"}).code == Code::None);
    CHECK(w.query->PayloadRequests().empty());
    const TerrainLayerWeightsResult failed = w.query->WeightsAt(point, request);
    CHECK(failed.status == Status::Failed);
    CHECK(failed.failure == Failure::LoadFailed);
    CHECK(failed.tile == tile);
    CHECK(failed.layers.empty());
    REQUIRE(w.query->TileError(tile));
    CHECK(w.query->TileError(tile)->code == Code::Io);
}

TEST_CASE("A weight query refuses foreign tiles and the wrong payload shape", "[terrain][query][weights]")
{
    Weighted w;
    TerrainReleaseInfo other = w.info;
    other.source = TerrainSourceFingerprint(0xBEEF);
    const TileCoord tile{0, 0};
    const std::shared_ptr<const TerrainDecodedWeights> payload = w.Load(tile);
    CHECK(w.query->Insert(other, payload).code == Code::WrongIdentity);
    CHECK(w.query->MarkFailed(other, tile, {Code::Io, "x"}).code == Code::WrongIdentity);
    CHECK(w.query->Insert(w.info, nullptr).code == Code::InvalidInput);

    const TileCoord plainTile{-1, -1};
    const auto wrongShape = std::make_shared<TerrainDecodedWeights>(*payload);
    wrongShape->tile = plainTile;
    wrongShape->intervals = payload->intervals + 1;
    CHECK(w.query->Insert(w.info, wrongShape).code == Code::InvalidInput);

    const TileCoord outside{9, 9};
    const auto foreign = std::make_shared<TerrainDecodedWeights>(*payload);
    foreign->tile = outside;
    CHECK(w.query->Insert(w.info, foreign).code == Code::NotFound);

    REQUIRE(w.query->Insert(w.info, payload).code == Code::None);
    w.query->Evict(tile);
    CHECK(w.query->Residency(tile) == TerrainTileResidency::Absent);
    CHECK(w.query->ResidentTile(tile) == nullptr);
}

TEST_CASE("World weight queries subtract the placement and keep the release identity", "[terrain][query][weights]")
{
    Weighted w;
    const TerrainPlacement placement{w.info.asset, *WorldPosition::FromDoubleMeters({25.5, -3.0, -40.25})};
    const TerrainLocalPosition point = Local(-58.25, 37.5);
    const std::shared_ptr<const TerrainDecodedWeights> payload = w.Load(AddressLocal(w.layout, point).tile);
    REQUIRE(w.query->Insert(w.info, payload).code == Code::None);

    const auto localMeters = point.Meters();
    const auto world = WorldPosition::FromDoubleMeters({localMeters.x + 25.5, 0.0, localMeters.z - 40.25});
    REQUIRE(world);
    const TerrainLayerWeightsResult result =
        w.query->WeightsAt(*world, placement, TerrainLayerWeightsRequest{w.info});
    CHECK(result.status == Status::Ready);
    CheckLayers(result.layers, w.query->WeightsAt(point, TerrainLayerWeightsRequest{w.info}).layers);

    const auto outside = WorldPosition::FromDoubleMeters({1000.0, 0.0, 0.0});
    REQUIRE(outside);
    CHECK(w.query->WeightsAt(*outside, placement, TerrainLayerWeightsRequest{w.info}).status == Status::OutsideMap);

    const TerrainPlacement otherAsset{TerrainAsset("terrain/other-weights.terrain.json"), placement.translation};
    const TerrainLayerWeightsResult wrong =
        w.query->WeightsAt(*world, otherAsset, TerrainLayerWeightsRequest{w.info});
    CHECK(wrong.status == Status::Failed);
    CHECK(wrong.failure == Failure::WrongRelease);
}
