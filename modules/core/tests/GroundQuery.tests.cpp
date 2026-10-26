/**
 * @file GroundQuery.tests.cpp
 * @author Jackey Cen
 * @brief Tests for core/GroundQuery.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <core/ecs/GroundQuery.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

namespace
{
    using mir::GroundQuery;
    using mir::GroundQueryResult;
    using mir::GroundQueryStatus;
    using mir::Registration;
    using mir::World;
    using mir::WorldPosition;

    WorldPosition At(double x, double y, double z)
    {
        return *WorldPosition::FromDoubleMeters({ x, y, z });
    }

    class FakeSource final : public mir::IGroundQuerySource
    {
      public:
        GroundQueryResult Sample(WorldPosition position) override
        {
            ++mSamples;
            GroundQueryResult result;
            result.status = mStatus;
            if (mStatus == GroundQueryStatus::Ready)
                result.surface = position;
            result.spacing = mSpacing;
            result.sourceSpacing = mSourceSpacing;
            result.identity = mIdentity;
            result.diagnostic = mDiagnostic;
            return result;
        }

        mir::GroundQueryCoverage Probe(WorldPosition) override
        {
            ++mProbes;
            mir::GroundQueryCoverage coverage;
            coverage.mCovered = mCovered;
            coverage.mSourceResident = mSourceResident;
            coverage.mSourceSpacing = mSourceSpacing;
            coverage.mIdentity = mIdentity;
            coverage.mDiagnostic = mDiagnostic;
            return coverage;
        }

        uint64_t Identity() const override { return mIdentity; }

        GroundQueryStatus mStatus = GroundQueryStatus::Ready;
        double mSpacing = 4.0;
        double mSourceSpacing = 4.0;
        uint64_t mIdentity = 11;
        std::string mDiagnostic;
        bool mCovered = true;
        bool mSourceResident = true;
        int mSamples = 0;
        int mProbes = 0;
    };
}

TEST_CASE("An unbound ground query fails instead of guessing a height", "[core][ground]")
{
    World world;
    GroundQuery &query = world.EmplaceResource<GroundQuery>();

    CHECK_FALSE(query.Bound());
    const GroundQueryResult result = query.Sample(At(1.0, 2.0, 3.0));
    CHECK(result.status == GroundQueryStatus::Failed);
    CHECK_FALSE(result.surface);
    CHECK_FALSE(result.diagnostic.empty());
}

TEST_CASE("Binding a ground source answers and unbinding it clears the source", "[core][ground]")
{
    World world;
    GroundQuery &query = world.EmplaceResource<GroundQuery>();
    FakeSource source;

    const uint64_t before = query.Revision();
    Registration binding = query.Bind(source);
    CHECK(query.Bound());
    CHECK(query.Revision() != before);
    CHECK(query.Identity() == 11);

    const GroundQueryResult result = query.Sample(At(1.0, 2.0, 3.0));
    REQUIRE(result.status == GroundQueryStatus::Ready);
    REQUIRE(result.surface);
    CHECK(source.mSamples == 1);

    binding.Reset();
    CHECK_FALSE(query.Bound());
    CHECK(query.Identity() == 0);
    CHECK_FALSE(query.Sample(At(0.0, 0.0, 0.0)).IsReady());
}

TEST_CASE("An old registration token leaves a newer binding alone", "[core][ground]")
{
    World world;
    GroundQuery &query = world.EmplaceResource<GroundQuery>();
    FakeSource first;
    FakeSource second;

    Registration stale = query.Bind(first);
    Registration current = query.Bind(second);
    stale.Reset();
    CHECK(query.Bound());
    CHECK(query.Identity() == second.mIdentity);

    current.Reset();
    CHECK_FALSE(query.Bound());
}

TEST_CASE("A coverage probe reports source spacing without sampling", "[core][ground]")
{
    World world;
    GroundQuery &query = world.EmplaceResource<GroundQuery>();
    FakeSource source;
    source.mSourceSpacing = 8.0;
    source.mSourceResident = false;
    const Registration binding = query.Bind(source);

    const mir::GroundQueryCoverage coverage = query.Probe(At(1.0, 2.0, 3.0));
    CHECK(coverage.mCovered);
    CHECK_FALSE(coverage.mSourceResident);
    CHECK(coverage.mSourceSpacing == 8.0);
    CHECK(source.mSamples == 0);
    CHECK(source.mProbes == 1);

    source.mCovered = false;
    CHECK_FALSE(query.Probe(At(1.0, 2.0, 3.0)).mCovered);
}

TEST_CASE("Ground diagnostics are bounded", "[core][ground]")
{
    World world;
    GroundQuery &query = world.EmplaceResource<GroundQuery>();
    FakeSource source;
    const Registration binding = query.Bind(source);
    source.mDiagnostic.assign(4096, 'x');

    CHECK(query.Sample(At(0.0, 0.0, 0.0)).diagnostic.size() <= 160);
}
