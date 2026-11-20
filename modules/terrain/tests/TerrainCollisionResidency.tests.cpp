/**
 * @file TerrainCollisionResidency.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainCollisionResidency.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"
#include "TerrainResidencyTestSupport.h"
#include "TerrainStreamingTestSupport.h"

#include <terrain/TerrainCollision.h>
#include <terrain/TerrainCollisionResidency.h>
#include <terrain/TerrainExecutor.h>
#include <terrain/TerrainHeightQuery.h>
#include <terrain/TerrainOverlay.h>
#include <terrain/TerrainScene.h>
#include <terrain/TerrainStreamer.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    using State = TerrainCollisionState;
    constexpr double kStep = 1.0 / 60.0;

    // Rolling ground: a resample coarser than the source misses its bends by a measurable amount.
    float Ground(double x, double z)
    {
        return static_cast<float>(10.0 + 2.0 * std::sin(0.15 * x) * std::cos(0.11 * z));
    }

    // The first tiles are clipped to start on an odd sample.
    TerrainLayout CollisionLayout()
    {
        return *TerrainLayout::CreateFromTileSize(64.0, 4.0, {-188.0, -188.0, 192.0, 192.0});
    }

    std::vector<TileHeights> CollisionTiles(const TerrainLayout &layout)
    {
        std::vector<TileHeights> tiles;
        const TileRange range = layout.Tiles();
        const int32_t n = layout.IntervalsPerTile();
        for (int32_t z = range.min.z; z <= range.max.z; ++z)
            for (int32_t x = range.min.x; x <= range.max.x; ++x)
            {
                TileHeights tile = *TileHeights::Create({x, z}, n);
                for (int32_t k = 0; k <= n; ++k)
                    for (int32_t i = 0; i <= n; ++i)
                        tile.Set(i, k,
                                 Ground(layout.SamplePosition(layout.TileFirstSample(x) + i),
                                        layout.SamplePosition(layout.TileFirstSample(z) + k)));
                tiles.push_back(std::move(tile));
            }
        return tiles;
    }

    TerrainEncodedRelease CollisionRelease(const TerrainReleaseInfo &info)
    {
        const TerrainLayout layout = CollisionLayout();
        return Encode(info, layout, CollisionTiles(layout));
    }

    const TileHeights &TileIn(const std::vector<TileHeights> &tiles, TileCoord coord)
    {
        const auto found = std::find_if(tiles.begin(), tiles.end(),
                                        [coord](const TileHeights &tile) { return tile.Tile() == coord; });
        REQUIRE(found != tiles.end());
        return *found;
    }

    TileHeights &TileIn(std::vector<TileHeights> &tiles, TileCoord coord)
    {
        return const_cast<TileHeights &>(TileIn(static_cast<const std::vector<TileHeights> &>(tiles), coord));
    }

    // Copies the release tiles with one tile's interior raised; its edges stay shared with the neighbours.
    std::vector<TileHeights> EditedTile(const std::vector<TileHeights> &tiles, TileCoord coord, float delta)
    {
        std::vector<TileHeights> edited = tiles;
        TileHeights &tile = TileIn(edited, coord);
        const int32_t n = tile.IntervalsPerTile();
        for (int32_t k = 1; k < n; ++k)
            for (int32_t i = 1; i < n; ++i)
                tile.Set(i, k, tile.At(i, k) + delta);
        tile.Set(8, 8, tile.At(8, 8) + delta);
        return edited;
    }

    std::shared_ptr<const TerrainOverlay> OverlayFrom(const std::vector<TileHeights> &tiles, AssetId asset,
                                                      uint64_t revision, std::initializer_list<TileCoord> coords)
    {
        std::unordered_map<TileCoord, std::shared_ptr<const TileHeights>> held;
        for (const TileCoord coord : coords)
            held.emplace(coord, std::make_shared<const TileHeights>(TileIn(tiles, coord)));
        return std::make_shared<const TerrainOverlay>(asset, revision, std::move(held));
    }

    WorldPosition At(const glm::dvec3 &meters)
    {
        const std::optional<WorldPosition> position = WorldPosition::FromDoubleMeters(meters);
        REQUIRE(position);
        return *position;
    }

    TerrainCollisionSource Falling(const glm::dvec3 &at, double speed = 0.0, double tolerance = 0.05)
    {
        TerrainCollisionSource source;
        source.position = At(at);
        source.velocity = {0.0, -speed, 0.0};
        source.radius = 0.5;
        source.acceleration = 9.81;
        source.tolerance = tolerance;
        return source;
    }

    struct Fixture
    {
        ManualClock clock;
        std::shared_ptr<TerrainManualExecutor> executor = std::make_shared<TerrainManualExecutor>();
        TerrainStreamingSettings settings;
        std::unique_ptr<TerrainStreamer> streamer;
        std::shared_ptr<FakePhysics> physics = std::make_shared<FakePhysics>();
        std::unique_ptr<TerrainCollisionResidency> residency;
        TerrainReleaseInfo info;
        ControlledRelease release;

        explicit Fixture(TerrainResidencyMode mode, uint64_t releaseId = 0xC011,
                         TerrainStreamingSettings::Collision collision = StreamingSettings(1).collision)
            : settings(StreamingSettings(uint64_t(1) << 30, mode)),
              info(Info(TerrainAsset("terrain/collision.terrain.json"), releaseId))
        {
            settings.collision = collision;
            streamer = std::make_unique<TerrainStreamer>(settings, executor, clock.Function());
            residency = std::make_unique<TerrainCollisionResidency>(settings.collision, executor, physics);
            release = OpenControlled(CollisionRelease(info));
            streamer->Add(release.release);
        }

        void Place(std::vector<glm::dvec3> placements)
        {
            std::vector<TerrainCollisionInstance> instances;
            for (const glm::dvec3 &at : placements)
                instances.push_back({release.release->SharedHandle(), {info.asset, At(at)}, *streamer->ModeOf(info)});
            residency->SetInstances(instances);
        }

        double Now() const
        {
            return *clock.now;
        }

        void Step(std::span<const TerrainCollisionSource> sources)
        {
            streamer->Update();
            residency->Install(Now());
            residency->Update(sources, *streamer, Now(), kStep);
            executor->RunAll();
            clock.Advance(kStep);
        }

        void Run(int steps, std::span<const TerrainCollisionSource> sources)
        {
            for (int i = 0; i < steps; ++i)
                Step(sources);
        }

        std::set<std::pair<int32_t, int32_t>> PlacedPieces(bool coarse = false) const
        {
            std::set<std::pair<int32_t, int32_t>> pieces;
            for (const auto &[id, placed] : physics->placed)
                if ((placed.shape->spacing > 4.0) == coarse)
                    pieces.insert({int32_t(std::floor(placed.shape->origin.Meters().x / 128.0)),
                                   int32_t(std::floor(placed.shape->origin.Meters().z / 128.0))});
            return pieces;
        }
    };

    const FakeShape *FineShapeAt(const Fixture &fixture, double x, double z)
    {
        for (const auto &[id, placed] : fixture.physics->placed)
            if (placed.shape->spacing < 8.0 && std::abs(placed.origin.Meters().x - x) < 0.5 &&
                std::abs(placed.origin.Meters().z - z) < 0.5)
                return placed.shape;
        return nullptr;
    }

    TerrainColliderId FineIdAt(const Fixture &fixture, double x, double z)
    {
        for (const auto &[id, placed] : fixture.physics->placed)
            if (placed.shape->spacing < 8.0 && std::abs(placed.origin.Meters().x - x) < 0.5 &&
                std::abs(placed.origin.Meters().z - z) < 0.5)
                return id;
        return 0;
    }

    bool AnyColliderAt(const Fixture &fixture, double x, double z)
    {
        return std::any_of(fixture.physics->placed.begin(), fixture.physics->placed.end(),
                           [&](const auto &placed) {
                               return std::abs(placed.second.origin.Meters().x - x) < 0.5 &&
                                      std::abs(placed.second.origin.Meters().z - z) < 0.5;
                           });
    }

    // The uniform samples a whole-map build gives the edited release's node, bit for bit.
    void CheckHeightsMatch(const Fixture &fixture, const std::vector<TileHeights> &tiles, const FakeShape &shape)
    {
        const TerrainLayout layout = CollisionLayout();
        const OpenedRelease edited = OpenInMemory(Encode(fixture.info, layout, tiles));
        TerrainHeightQuery query = TerrainHeightQuery::Create(edited.handle);
        const TerrainPreloadReport preload = PreloadAllTerrainTiles(query, edited.handle);
        INFO(preload.firstError.message);
        REQUIRE(preload.Ok());
        const TerrainSourceTileLookup lookup = [&query](uint32_t layer, TileCoord tile) {
            return query.ResidentTile(layer, tile);
        };
        const TerrainResult<TerrainUniformSamples> expected =
            BuildTerrainUniformSamples(edited.handle.Index(), lookup, shape.spacing, layout.Bounds());
        INFO(expected.error.message);
        REQUIRE(expected);
        const TerrainAreaSamples &area = expected.value->area;
        size_t compared = 0;
        for (uint32_t k = 0; k < shape.countZ; ++k)
            for (uint32_t i = 0; i < shape.countX; ++i)
            {
                const float height = shape.heights[size_t(k) * shape.countX + i];
                if (height == kTerrainColliderHole)
                    continue;
                const int64_t gx = shape.first.x + i, gz = shape.first.z + k;
                REQUIRE(gx >= area.samples.min.x);
                REQUIRE(gx <= area.samples.max.x);
                REQUIRE(gz >= area.samples.min.z);
                REQUIRE(gz <= area.samples.max.z);
                const size_t at = size_t(gz - area.samples.min.z) * area.countX + size_t(gx - area.samples.min.x);
                INFO("sample " << gx << ", " << gz);
                CHECK(height == area.heights[at]);
                ++compared;
            }
        CHECK(compared > 0);
    }
} // namespace

TEST_CASE("An Eager release collides as one piece, ready only once its colliders are placed", "[terrain][collision]")
{
    Fixture f(TerrainResidencyMode::Eager);
    f.Place({{0.0, 0.0, 0.0}, {1000.0, 5.0, 0.0}});
    const std::vector<TerrainCollisionSource> sources{Falling({10.0, 12.5, 10.0}), Falling({1010.0, 17.5, 10.0})};

    f.residency->Update(sources, *f.streamer, f.Now(), kStep);
    CHECK(f.residency->Answer(0).status.state == State::Pending);
    CHECK(f.residency->Answer(0).status.needed == 1);
    CHECK(f.residency->Answer(0).contacts.empty());

    f.Run(4, sources);
    REQUIRE(f.physics->placed.size() == 2);
    CHECK(f.physics->builds == 2);
    std::set<double> originsX;
    for (const auto &[id, placed] : f.physics->placed)
    {
        CHECK(placed.shape == f.physics->placed.begin()->second.shape);
        originsX.insert(double(placed.origin.Meters().x));
        CHECK(placed.shape->spacing == 4.0);
    }
    CHECK(originsX == std::set<double>{-188.0, 812.0});
    for (size_t s = 0; s < sources.size(); ++s)
    {
        const TerrainCollisionAnswer &answer = f.residency->Answer(s);
        CHECK(answer.status.state == State::Ready);
        CHECK(answer.status.ready == 1);
        CHECK(answer.status.contactError <= f.settings.collision.maxErrorMeters);
        CHECK(answer.contacts.size() == 1);
        CHECK_FALSE(answer.unowned);
    }
    CHECK(f.residency->Answer(0).contacts != f.residency->Answer(1).contacts);

    const std::vector<TerrainCollisionEvent> events = f.residency->TakeEvents();
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == TerrainCollisionEvent::Kind::Placed);
    CHECK(events[0].piece.whole);
    CHECK(events[0].colliders == 2);
    CHECK(std::isfinite(events[0].latencySeconds));
}

TEST_CASE("A slow Eager build does not stretch the prediction of every source", "[terrain][collision]")
{
    Fixture f(TerrainResidencyMode::Eager);
    f.Place({{0.0, 0.0, 0.0}});
    const std::vector<TerrainCollisionSource> sources{Falling({10.0, 12.5, 10.0})};
    const double prediction = f.residency->PredictionSeconds();

    f.Step(sources);
    f.clock.Advance(100.0 * prediction);
    f.Run(4, sources);
    REQUIRE(f.residency->Answer(0).status.state == State::Ready);

    const std::vector<TerrainCollisionEvent> events = f.residency->TakeEvents();
    REQUIRE(events.size() == 1);
    CHECK(events[0].piece.whole);
    CHECK(events[0].latencySeconds >= 100.0 * prediction);
    CHECK(f.residency->PredictionSeconds() == prediction);
}

TEST_CASE("Interest pieces are built only where a source's predicted range reaches the ground", "[terrain][collision]")
{
    Fixture f(TerrainResidencyMode::Interest);
    f.Place({{0.0, 0.0, 0.0}});
    const double reach = f.residency->PredictionSeconds();

    const std::vector<TerrainCollisionSource> high{Falling({100.0, 400.0, 100.0})};
    f.Run(10, high);
    CHECK(f.residency->Answer(0).status.state == State::Ready);
    CHECK(f.residency->Answer(0).status.needed == 0);
    CHECK(f.physics->builds == 0);
    CHECK(f.release.source->RangeRequests().empty());

    const std::vector<TerrainCollisionSource> diving{Falling({100.0, 400.0, 100.0}, 400.0 / reach)};
    f.Step(diving);
    CHECK(f.residency->Answer(0).status.state == State::Pending);
    CHECK(f.residency->Answer(0).status.needed == 1);
    f.Run(6, diving);
    CHECK(f.residency->Answer(0).status.state == State::Ready);
    CHECK(f.PlacedPieces() == std::set<std::pair<int32_t, int32_t>>{{0, 0}});
    CHECK_FALSE(f.release.source->RangeRequests().empty());
    CHECK(f.residency->Answer(0).contacts.size() == 1);
}

TEST_CASE("A fast source needs the pieces ahead of it and prefetches the ones beyond", "[terrain][collision]")
{
    Fixture f(TerrainResidencyMode::Interest);
    f.Place({{0.0, 0.0, 0.0}});
    TerrainCollisionSource skimming = Falling({-150.0, 12.5, 100.0});
    skimming.velocity = {64.0 / f.residency->PredictionSeconds(), 0.0, 0.0};
    const std::vector<TerrainCollisionSource> sources{skimming};

    f.Run(8, sources);
    CHECK(f.residency->Answer(0).status.state == State::Ready);
    CHECK(f.residency->Answer(0).status.needed == 2);
    const std::set<std::pair<int32_t, int32_t>> placed = f.PlacedPieces();
    CHECK(placed.contains({-2, 0}));
    CHECK(placed.contains({-1, 0}));
    CHECK(placed.contains({0, 0}));
    CHECK_FALSE(placed.contains({1, 0}));
    CHECK_FALSE(placed.contains({-2, -2}));
}

TEST_CASE("A coarse piece within a source's tolerance lets it simulate while the fine piece builds",
          "[terrain][collision]")
{
    Fixture f(TerrainResidencyMode::Interest);
    f.Place({{0.0, 0.0, 0.0}});
    const std::vector<TerrainCollisionSource> sources{Falling({100.0, 13.0, 100.0}, 0.0, 2.0),
                                                      Falling({110.0, 13.0, 110.0}, 0.0, 0.05)};
    f.Run(2, sources);
    for (;;)
    {
        f.streamer->Update();
        f.residency->Install(f.Now());
        f.residency->Update(sources, *f.streamer, f.Now(), kStep);
        if (f.residency->Answer(0).status.state != State::Pending)
            break;
        REQUIRE(f.executor->RunOne());
        f.clock.Advance(kStep);
    }
    const TerrainCollisionAnswer &loose = f.residency->Answer(0);
    CHECK(loose.status.state == State::CoarseSufficient);
    CHECK(loose.status.coarse == 1);
    CHECK(loose.status.contactError > f.settings.collision.maxErrorMeters);
    CHECK(loose.status.contactError <= 2.0);
    CHECK(loose.contacts.size() == 1);
    CHECK(f.residency->Answer(1).status.state == State::Pending);
    CHECK(f.PlacedPieces(true) == std::set<std::pair<int32_t, int32_t>>{{0, 0}});

    f.Run(4, sources);
    CHECK(f.residency->Answer(0).status.state == State::Ready);
    CHECK(f.residency->Answer(1).status.state == State::Ready);
    CHECK(f.residency->Answer(0).status.contactError <= f.settings.collision.maxErrorMeters);
    f.Run(int(f.settings.collision.retainSeconds / kStep) + 2, sources);
    CHECK(f.PlacedPieces(true).empty());
    CHECK(f.PlacedPieces() == std::set<std::pair<int32_t, int32_t>>{{0, 0}});
}

TEST_CASE("Slow and failed reads keep a source pending or failed, never ready, until the heights load",
          "[terrain][collision]")
{
    Fixture f(TerrainResidencyMode::Interest);
    f.Place({{0.0, 0.0, 0.0}});
    const std::vector<TerrainCollisionSource> sources{Falling({100.0, 13.0, 100.0})};

    f.release.source->Hold();
    f.Step(sources);
    f.streamer->Update();
    for (int i = 0; i < 30; ++i)
    {
        f.residency->Install(f.Now());
        f.residency->Update(sources, *f.streamer, f.Now(), kStep);
        CHECK(f.residency->Answer(0).status.state == State::Pending);
        f.clock.Advance(kStep);
    }
    f.release.source->Resume();
    f.Run(4, sources);
    CHECK(f.residency->Answer(0).status.state == State::Ready);
    const std::vector<TerrainCollisionEvent> events = f.residency->TakeEvents();
    const auto placed = std::find_if(events.begin(), events.end(), [](const TerrainCollisionEvent &e) {
        return e.kind == TerrainCollisionEvent::Kind::Placed && !e.piece.coarse;
    });
    REQUIRE(placed != events.end());
    CHECK(placed->latencySeconds >= 30 * kStep);
    CHECK(f.residency->PredictionSeconds() >=
          placed->latencySeconds + f.settings.collision.latencyMarginSeconds - 1e-9);

    Fixture failing(TerrainResidencyMode::Interest, 0xFA11);
    failing.Place({{0.0, 0.0, 0.0}});
    failing.release.source->FailNext("chunk_0_0.tpack", ~uint32_t(0));
    State seen = State::Ready;
    for (int i = 0; i < 60 && seen != State::Failed; ++i)
    {
        failing.Step(sources);
        seen = failing.residency->Answer(0).status.state;
        CHECK(seen != State::Ready);
    }
    CHECK(seen == State::Failed);
    CHECK(failing.residency->Answer(0).status.reason.starts_with("collision piece (0, 0)"));
    failing.release.source->FailNext("chunk_0_0.tpack", 0);
    failing.clock.Advance(failing.settings.retry.failedHoldSeconds);
    failing.Run(10, sources);
    CHECK(failing.residency->Answer(0).status.state == State::Ready);
}

TEST_CASE("Pieces nobody needs retire after retainSeconds, and needed pieces stay past the budget",
          "[terrain][collision]")
{
    TerrainStreamingSettings::Collision tight = StreamingSettings(1).collision;
    tight.budgetBytes = 1;
    Fixture f(TerrainResidencyMode::Interest, 0xB0D6, tight);
    f.Place({{0.0, 0.0, 0.0}});
    std::vector<TerrainCollisionSource> sources{Falling({100.0, 13.0, 100.0}), Falling({-150.0, 13.0, -150.0})};
    f.Run(8, sources);
    CHECK(f.residency->Answer(0).status.state == State::Ready);
    CHECK(f.residency->Answer(1).status.state == State::Ready);
    CHECK(f.residency->Stats().overBudgetBytes > 0);
    const size_t before = f.physics->placed.size();

    sources.pop_back();
    f.Run(int(tight.retainSeconds / kStep) + 2, sources);
    CHECK(f.physics->placed.size() < before);
    CHECK(f.PlacedPieces().contains({0, 0}));
    CHECK_FALSE(f.PlacedPieces().contains({-2, -2}));
    const std::vector<TerrainCollisionEvent> events = f.residency->TakeEvents();
    CHECK(std::any_of(events.begin(), events.end(),
                      [](const TerrainCollisionEvent &e) { return e.kind == TerrainCollisionEvent::Kind::Retired; }));
    CHECK(f.residency->Answer(0).status.state == State::Ready);
}

TEST_CASE("Sources share pieces, and a piece stays while any of them needs it", "[terrain][collision]")
{
    Fixture f(TerrainResidencyMode::Interest);
    f.Place({{0.0, 0.0, 0.0}});
    std::vector<TerrainCollisionSource> sources{Falling({100.0, 13.0, 100.0}), Falling({110.0, 13.0, 120.0})};
    f.Run(8, sources);
    CHECK(f.residency->Answer(0).contacts == f.residency->Answer(1).contacts);
    const int builds = f.physics->builds;

    sources.erase(sources.begin());
    f.Run(int(f.settings.collision.retainSeconds / kStep) + 2, sources);
    CHECK(f.PlacedPieces().contains({0, 0}));
    CHECK(f.physics->builds == builds);
}

TEST_CASE("A source touches the pieces that hold its footprint, and every piece near it when none does",
          "[terrain][collision]")
{
    std::vector<TerrainCollisionPieceArea> areas(2);
    areas[0].rect = {0.0, 0.0, 192.0, 128.0};
    areas[0].seams[1] = true;
    areas[0].margin = 8.0;
    areas[1].rect = {128.0, 0.0, 256.0, 128.0};
    areas[1].seams[0] = true;
    areas[1].margin = 8.0;
    bool unowned = false;
    CHECK(TerrainCollisionOwners(areas, {10.0, 10.0, 12.0, 12.0}, unowned) == std::vector<size_t>{0});
    CHECK(TerrainCollisionOwners(areas, {-50.0, 10.0, -40.0, 12.0}, unowned) == std::vector<size_t>{0});
    CHECK_FALSE(unowned);
    CHECK(TerrainCollisionOwners(areas, {150.0, 10.0, 152.0, 12.0}, unowned) == std::vector<size_t>{0, 1});
    CHECK(TerrainCollisionOwners(areas, {183.0, 10.0, 186.0, 12.0}, unowned) == std::vector<size_t>{1});
    CHECK(TerrainCollisionOwners(areas, {130.0, 10.0, 132.0, 12.0}, unowned) == std::vector<size_t>{0});
    CHECK(TerrainCollisionOwners(areas, {300.0, -10.0, 310.0, 200.0}, unowned) == std::vector<size_t>{1});
    CHECK_FALSE(unowned);
    CHECK(TerrainCollisionOwners(areas, {120.0, 10.0, 200.0, 12.0}, unowned) == std::vector<size_t>{0, 1});
    CHECK(unowned);

    Fixture f(TerrainResidencyMode::Interest);
    f.Place({{0.0, 0.0, 0.0}});
    const std::vector<TerrainCollisionSource> band{Falling({160.0, 13.0, 100.0})};
    f.Run(8, band);
    CHECK(f.residency->Answer(0).contacts.size() == 2);
    CHECK_FALSE(f.residency->Answer(0).unowned);
    const std::vector<TerrainCollisionSource> core{Falling({100.0, 13.0, 100.0})};
    f.Run(2, core);
    CHECK(f.residency->Answer(0).contacts.size() == 1);
}

TEST_CASE("Pieces start on an even sample and share one quantization range", "[terrain][collision]")
{
    Fixture f(TerrainResidencyMode::Interest);
    f.Place({{0.0, 0.0, 0.0}});
    const std::vector<TerrainCollisionSource> sources{Falling({-150.0, 13.0, -150.0}), Falling({100.0, 13.0, 100.0}),
                                                      Falling({170.0, 13.0, 170.0})};
    f.Run(8, sources);
    REQUIRE(f.physics->placed.size() >= 3);
    CHECK(f.PlacedPieces().contains({-2, -2}));
    const FakeShape *first = f.physics->placed.begin()->second.shape;
    for (const auto &[id, placed] : f.physics->placed)
    {
        CHECK(placed.shape->first.x % 2 == 0);
        CHECK(placed.shape->first.z % 2 == 0);
        CHECK(placed.shape->quantizationMin == first->quantizationMin);
        CHECK(placed.shape->quantizationMax == first->quantizationMax);
    }
}

TEST_CASE("Changing the instances drops the colliders of placements and releases that left", "[terrain][collision]")
{
    Fixture f(TerrainResidencyMode::Eager);
    f.Place({{0.0, 0.0, 0.0}, {1000.0, 0.0, 0.0}});
    const std::vector<TerrainCollisionSource> none;
    f.Run(4, none);
    CHECK(f.physics->placed.size() == 2);

    f.Place({{0.0, 0.0, 0.0}});
    CHECK(f.physics->placed.size() == 1);
    f.residency->SetInstances({});
    CHECK(f.physics->placed.empty());
    f.Run(4, none);
    CHECK(f.physics->placed.empty());

    f.Place({{0.0, 0.0, 0.0}});
    f.residency->Update(none, *f.streamer, f.Now(), kStep);
    f.residency->SetInstances({});
    f.executor->RunAll();
    f.Run(2, none);
    CHECK(f.physics->placed.empty());
}

TEST_CASE("A piece whose heights make no shape fails once and the source never reads ready", "[terrain][collision]")
{
    Fixture f(TerrainResidencyMode::Interest);
    f.Place({{0.0, 0.0, 0.0}});
    f.physics->refuse = true;
    const std::vector<TerrainCollisionSource> sources{Falling({100.0, 13.0, 100.0})};
    f.Run(8, sources);
    CHECK(f.residency->Answer(0).status.state == State::Failed);
    CHECK(f.residency->Answer(0).status.reason.find("no height field") != std::string::npos);
    const int builds = f.physics->builds;
    f.Run(8, sources);
    CHECK(f.physics->builds == builds);
    const std::vector<TerrainCollisionEvent> events = f.residency->TakeEvents();
    CHECK(std::count_if(events.begin(), events.end(), [](const TerrainCollisionEvent &e) {
              return e.kind == TerrainCollisionEvent::Kind::Failed;
          }) >= 1);
}

TEST_CASE("An overlay rebuilds only the pieces covering its tiles, from the edited heights",
          "[terrain][collision][overlay]")
{
    Fixture f(TerrainResidencyMode::Interest);
    f.Place({{0.0, 0.0, 0.0}});
    const std::vector<TerrainCollisionSource> sources{Falling({100.0, 13.0, 100.0}),
                                                      Falling({170.0, 13.0, 170.0})};
    f.Run(10, sources);
    REQUIRE(f.residency->Answer(0).status.state == State::Ready);
    REQUIRE(f.residency->Answer(1).status.state == State::Ready);
    const TerrainColliderId affectedId = FineIdAt(f, 0.0, 0.0);
    const TerrainColliderId untouchedId = FineIdAt(f, 128.0, 128.0);
    REQUIRE(affectedId != 0);
    REQUIRE(untouchedId != 0);
    const std::vector<float> baseHeights = FineShapeAt(f, 0.0, 0.0)->heights;
    const int builds = f.physics->builds;

    const std::vector<TileHeights> tiles = CollisionTiles(CollisionLayout());
    const std::vector<TileHeights> edited = EditedTile(tiles, {0, 0}, 1.5f);
    f.residency->SetOverlay(f.info.asset, OverlayFrom(edited, f.info.asset, 1, {{0, 0}}));
    CHECK(f.physics->placed.contains(affectedId));

    for (int i = 0; i < 20 && FineIdAt(f, 0.0, 0.0) == affectedId; ++i)
    {
        CHECK(AnyColliderAt(f, 0.0, 0.0));
        f.Step(sources);
    }
    CHECK(AnyColliderAt(f, 0.0, 0.0));
    INFO(f.residency->Answer(0).status.reason);
    CHECK(FineIdAt(f, 0.0, 0.0) != affectedId);
    CHECK(FineIdAt(f, 128.0, 128.0) == untouchedId);
    CHECK(f.physics->builds <= builds + 2);

    const FakeShape *rebuilt = FineShapeAt(f, 0.0, 0.0);
    REQUIRE(rebuilt);
    CHECK(rebuilt->heights != baseHeights);
    CheckHeightsMatch(f, edited, *rebuilt);
    f.Run(2, sources);
    CHECK(f.residency->Answer(0).status.state == State::Ready);
}

TEST_CASE("A snapshot swap keeps the old collider placed and touchable until the new one is installed",
          "[terrain][collision][overlay]")
{
    Fixture f(TerrainResidencyMode::Interest);
    f.Place({{0.0, 0.0, 0.0}});
    const std::vector<TerrainCollisionSource> sources{Falling({100.0, 13.0, 100.0})};
    f.Run(8, sources);
    f.Run(int(f.settings.collision.retainSeconds / kStep) + 2, sources);
    REQUIRE(f.residency->Answer(0).status.state == State::Ready);
    REQUIRE(f.physics->placed.size() == 1);
    const int removedBefore = f.physics->removed;
    const TerrainColliderId oldId = FineIdAt(f, 0.0, 0.0);
    REQUIRE(oldId != 0);

    const std::vector<TileHeights> tiles = CollisionTiles(CollisionLayout());
    f.physics->ops.clear();
    f.residency->SetOverlay(f.info.asset,
                            OverlayFrom(EditedTile(tiles, {0, 0}, 1.5f), f.info.asset, 1, {{0, 0}}));
    CHECK(FineIdAt(f, 0.0, 0.0) == oldId);

    f.streamer->Update();
    f.residency->Update(sources, *f.streamer, f.Now(), kStep);
    CHECK(f.residency->Answer(0).status.state == State::Ready);
    CHECK(FineIdAt(f, 0.0, 0.0) == oldId);
    const TerrainCollisionAnswer &waiting = f.residency->Answer(0);
    CHECK(std::find(waiting.contacts.begin(), waiting.contacts.end(), oldId) != waiting.contacts.end());

    f.executor->RunAll();
    CHECK(FineIdAt(f, 0.0, 0.0) == oldId);
    CHECK(f.physics->removed == removedBefore);

    f.residency->Install(f.Now());
    const TerrainColliderId newId = FineIdAt(f, 0.0, 0.0);
    REQUIRE(newId != 0);
    CHECK(newId != oldId);
    CHECK_FALSE(f.physics->placed.contains(oldId));
    CHECK(f.physics->removed == removedBefore + 1);
    const auto place = std::find(f.physics->ops.begin(), f.physics->ops.end(), std::pair{true, newId});
    const auto remove = std::find(f.physics->ops.begin(), f.physics->ops.end(), std::pair{false, oldId});
    REQUIRE(place != f.physics->ops.end());
    REQUIRE(remove != f.physics->ops.end());
    CHECK(place < remove);

    f.streamer->Update();
    f.residency->Update(sources, *f.streamer, f.Now(), kStep);
    CHECK(f.residency->Answer(0).status.state == State::Ready);
    const TerrainCollisionAnswer &swapped = f.residency->Answer(0);
    CHECK(std::find(swapped.contacts.begin(), swapped.contacts.end(), newId) != swapped.contacts.end());
}

TEST_CASE("Replacing or clearing a snapshot rebuilds only the pieces around both snapshots",
          "[terrain][collision][overlay]")
{
    Fixture f(TerrainResidencyMode::Interest);
    f.Place({{0.0, 0.0, 0.0}});
    const std::vector<TerrainCollisionSource> sources{Falling({100.0, 13.0, 100.0}),
                                                      Falling({170.0, 13.0, 170.0}),
                                                      Falling({-150.0, 13.0, -150.0})};
    f.Run(10, sources);
    for (size_t s = 0; s < sources.size(); ++s)
        REQUIRE(f.residency->Answer(s).status.state == State::Ready);
    const TerrainColliderId originPiece = FineIdAt(f, 0.0, 0.0);
    const TerrainColliderId farPiece = FineIdAt(f, 128.0, 128.0);
    const TerrainColliderId untouchedPiece = FineIdAt(f, -192.0, -192.0);
    REQUIRE(originPiece != 0);
    REQUIRE(farPiece != 0);
    REQUIRE(untouchedPiece != 0);

    const std::vector<TileHeights> tiles = CollisionTiles(CollisionLayout());
    const std::vector<TileHeights> first = EditedTile(tiles, {0, 0}, 1.0f);
    f.residency->SetOverlay(f.info.asset, OverlayFrom(first, f.info.asset, 1, {{0, 0}}));
    for (int i = 0; i < 20 && FineIdAt(f, 0.0, 0.0) == originPiece; ++i)
        f.Step(sources);
    INFO(f.residency->Answer(0).status.reason);
    CHECK(FineIdAt(f, 0.0, 0.0) != originPiece);
    CHECK(FineIdAt(f, 128.0, 128.0) == farPiece);
    CHECK(FineIdAt(f, -192.0, -192.0) == untouchedPiece);
    CheckHeightsMatch(f, first, *FineShapeAt(f, 0.0, 0.0));

    const std::vector<TileHeights> second = EditedTile(tiles, {-3, -3}, 1.0f);
    f.residency->SetOverlay(f.info.asset, OverlayFrom(second, f.info.asset, 2, {{-3, -3}}));
    const TerrainColliderId editedId = FineIdAt(f, 0.0, 0.0);
    for (int i = 0; i < 20 && FineIdAt(f, 0.0, 0.0) == editedId; ++i)
        f.Step(sources);
    for (int i = 0; i < 20 && FineIdAt(f, -192.0, -192.0) == untouchedPiece; ++i)
        f.Step(sources);
    INFO(f.residency->Answer(0).status.reason);
    INFO(f.residency->Answer(2).status.reason);
    CHECK(FineIdAt(f, 0.0, 0.0) != editedId);
    CHECK(FineIdAt(f, -192.0, -192.0) != untouchedPiece);
    CHECK(FineIdAt(f, 128.0, 128.0) == farPiece);
    CheckHeightsMatch(f, tiles, *FineShapeAt(f, 0.0, 0.0));
    CheckHeightsMatch(f, second, *FineShapeAt(f, -192.0, -192.0));

    const TerrainColliderId restoredId = FineIdAt(f, 0.0, 0.0);
    const TerrainColliderId secondId = FineIdAt(f, -192.0, -192.0);
    f.residency->SetOverlay(f.info.asset, nullptr);
    for (int i = 0; i < 20 && FineIdAt(f, -192.0, -192.0) == secondId; ++i)
        f.Step(sources);
    CHECK(FineIdAt(f, 0.0, 0.0) == restoredId);
    CHECK(FineIdAt(f, -192.0, -192.0) != secondId);
    CHECK(FineIdAt(f, 128.0, 128.0) == farPiece);
    CheckHeightsMatch(f, tiles, *FineShapeAt(f, -192.0, -192.0));
}
