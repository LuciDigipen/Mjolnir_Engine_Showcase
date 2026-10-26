/**
 * @file SnapToTerrain.tests.cpp
 * @author Jackey Cen
 * @brief Tests for editor/SnapToTerrain.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "EditingFixture.h"

#include <editor/SnapToTerrain.h>
#include <editor/Selection.h>
#include <editor/history/AuthoredView.h>
#include <editor/history/Authoring.h>
#include <editor/history/History.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/GroundQuery.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Name.h>
#include <core/ecs/components/Transform.h>
#include <renderer/components/MeshBounds.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using mir::AuthoredAction;
    using mir::AuthoredComponentState;
    using mir::AuthoredEntry;
    using mir::AuthoredFieldState;
    using mir::AuthoredInstanceRole;
    using mir::AuthoredKey;
    using mir::AuthoringWrite;
    using mir::ComponentRegistry;
    using mir::DocumentId;
    using mir::Entity;
    using mir::EngineMode;
    using mir::EngineModeState;
    using mir::FieldKind;
    using mir::GroundQuery;
    using mir::GroundQueryResult;
    using mir::GroundQueryStatus;
    using mir::History;
    using mir::kNullEntity;
    using mir::MeshBounds;
    using mir::Selection;
    using mir::SnapToTerrainAction;
    using mir::SnapToTerrainEdit;
    using mir::SnapToTerrainSession;
    using mir::Transform;
    using mir::World;
    using mir::WorldPosition;
    using mir::WorldVec3;

    WorldPosition At(double x, double y, double z)
    {
        return *WorldPosition::FromDoubleMeters({x, y, z});
    }

    WorldVec3 Vec(double x, double y, double z)
    {
        return *mir::TryWorldVec3FromDouble({x, y, z});
    }

    class FakeGround final : public mir::IGroundQuerySource
    {
      public:
        GroundQueryResult Sample(WorldPosition position) override
        {
            ++mCalls;
            GroundQueryResult result;
            result.spacing = mSpacing;
            result.sourceSpacing = mSpacing;
            result.identity = mIdentity;
            if (mLoading)
            {
                result.status = GroundQueryStatus::Loading;
                result.diagnostic = "Waiting for the test source.";
                return result;
            }

            const glm::dvec3 meters(position.Meters());
            const bool second = mChangeAfterCalls >= 0 && mCalls > mChangeAfterCalls;
            const double a = second ? mA2 : mA;
            const double b = second ? mB2 : mB;
            const double c = second ? mC2 : mC;
            result.status = GroundQueryStatus::Ready;
            result.surface = WorldPosition::FromDoubleMeters({meters.x, c + a * meters.x + b * meters.z, meters.z});
            return result;
        }

        mir::GroundQueryCoverage Probe(WorldPosition) override
        {
            mir::GroundQueryCoverage coverage;
            coverage.mCovered = mCovered;
            coverage.mSourceResident = !mLoading;
            coverage.mSourceSpacing = mSpacing;
            coverage.mIdentity = mIdentity;
            coverage.mDiagnostic = mCovered ? (mLoading ? "Waiting for the test source." : "") : "No terrain covers this point.";
            return coverage;
        }

        uint64_t Identity() const override { return mIdentity; }

        bool mLoading = false;
        bool mCovered = true;
        double mSpacing = 4.0;
        double mA = 0.0;
        double mB = 0.0;
        double mC = 5.0;
        uint64_t mIdentity = 77;
        int mCalls = 0;
        int mChangeAfterCalls = -1;
        double mA2 = 0.0;
        double mB2 = 0.0;
        double mC2 = 5.0;
    };

    struct Fixture
    {
        World mWorld;
        FakeGround mSource;
        GroundQuery *mGround = nullptr;
        mir::Registration mBinding;
        Entity mEntity = kNullEntity;

        Fixture(bool bounds = true)
        {
            mir::RegisterCoreComponents();
            mWorld.EmplaceResource<EngineModeState>();
            mWorld.EmplaceResource<Selection>();
            mGround = &mWorld.EmplaceResource<GroundQuery>();
            mBinding = mGround->Bind(mSource);
            mEntity = mWorld.CreateEntity();
            mir::AddTransform(mWorld, mEntity, Transform(WorldVec3{0.0}));
            mWorld.AddComponent(mEntity, mir::MakeName("Snap"));
            if (bounds)
                mWorld.AddComponent(mEntity, MeshBounds{glm::vec3(-2.0f, 0.0f, -2.0f), glm::vec3(2.0f, 1.0f, 2.0f)});
            Select(mEntity);
        }

        void Select(Entity entity) { mWorld.Resource<Selection>().mSelected = {entity}; }
        Transform &TransformOf() { return *mWorld.GetComponent<::mir::Transform>(mEntity); }
    };

    class FakeBackend final : public mir::IAuthoringBackend
    {
      public:
        std::optional<std::vector<AuthoredEntry>> Read(std::span<const AuthoredKey> keys) override
        {
            std::vector<AuthoredEntry> out;
            for (const AuthoredKey &key : keys)
                out.push_back(AuthoredEntry{key, At(key)});
            return out;
        }

        AuthoringWrite Write(std::span<const AuthoredEntry> entries) override
        {
            mWritten.clear();
            for (const AuthoredEntry &entry : entries)
            {
                std::erase_if(mValues, [&entry](const auto &stored) { return stored.first == entry.mKey; });
                if (entry.mValue)
                    mValues.emplace_back(entry.mKey, *entry.mValue);
                mWritten.push_back(entry.mKey);
            }
            ++mWrites;
            return AuthoringWrite{true, {}};
        }

        std::optional<std::string> At(const AuthoredKey &key) const
        {
            const auto it = std::find_if(mValues.begin(), mValues.end(),
                                         [&key](const auto &stored) { return stored.first == key; });
            return it != mValues.end() ? std::optional<std::string>(it->second) : std::nullopt;
        }

        std::vector<AuthoredKey> mWritten;
        int mWrites = 0;

      private:
        std::vector<std::pair<AuthoredKey, std::string>> mValues;
    };

    class FakeView final : public mir::IAuthoredEntities
    {
      public:
        std::optional<AuthoredKey> KeyOf(Entity entity, std::string_view component,
                                          std::string_view field) override
        {
            if (!mMember || entity != mTarget)
                return std::nullopt;
            return Key(entity, component, field);
        }

        std::optional<AuthoredKey> ComponentKeyOf(Entity entity, std::string_view component) override
        {
            if (!mMember || entity != mTarget)
                return std::nullopt;
            return Key(entity, component, {});
        }

        AuthoredFieldState FieldState(const AuthoredKey &) override { return AuthoredFieldState::Inherited; }
        AuthoredComponentState ComponentState(Entity, std::string_view) override
        {
            return AuthoredComponentState::Inherited;
        }
        std::vector<std::string> RemovedComponents(Entity) override { return {}; }
        AuthoredInstanceRole InstanceRole(Entity) override { return AuthoredInstanceRole::Ordinary; }
        std::string EncodeField(FieldKind, const void *) override { return "snapped"; }

        AuthoredAction Revert(const AuthoredKey &) override { return {}; }
        AuthoredAction Apply(const AuthoredKey &) override { return {}; }
        AuthoredAction CreateNode(Entity, std::size_t, const mir::EntityPresetDesc *) override { return {}; }
        AuthoredAction PlacePrefabNode(std::string_view, Entity, std::size_t,
                                       const std::optional<WorldVec3> &) override
        {
            return {};
        }
        AuthoredAction DeleteNodes(std::span<const Entity>) override { return {}; }
        AuthoredAction ReparentNode(Entity, Entity, std::size_t) override { return {}; }
        AuthoredAction CreatePrefab(Entity) override { return {}; }
        bool WritesComponentDirectly(Entity, std::string_view) override { return false; }

        bool mMember = false;
        Entity mTarget = kNullEntity;

      private:
        static AuthoredKey Key(Entity entity, std::string_view component, std::string_view field)
        {
            AuthoredKey key;
            key.mObject.mDocument = DocumentId{3, 4};
            key.mObject.mAddress = {mir::PackEntity(entity)};
            key.mComponent = component;
            key.mField = field;
            return key;
        }
    };
}

TEST_CASE("A plane fit is centroid relative and rejects degenerate spreads", "[editor][snap]")
{
    const auto planeHeight = [](double x, double z) { return 4.0 + 0.1 * x - 0.2 * z; };
    std::vector<glm::dvec3> points;
    for (const double x : {150000.5, 150010.5, 150000.5, 150010.5, 150005.5})
        for (const double z : {-75000.25, -74990.25})
            points.push_back({x, planeHeight(x, z), z});

    const std::optional<mir::SnapToTerrainPlane> plane = mir::FitSnapToTerrainPlane(points);
    REQUIRE(plane);
    CHECK(plane->mNormal.y == Catch::Approx(1.0 / std::sqrt(1.0 + 0.01 + 0.04)).epsilon(1e-9));
    CHECK(plane->mNormal.x == Catch::Approx(-0.1 / std::sqrt(1.0 + 0.01 + 0.04)).epsilon(1e-9));
    CHECK(plane->mNormal.z == Catch::Approx(0.2 / std::sqrt(1.0 + 0.01 + 0.04)).epsilon(1e-9));
    CHECK(plane->mResidual < 1e-9);

    const std::vector<glm::dvec3> collinear{{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}, {2.0, 2.0, 2.0}};
    CHECK_FALSE(mir::FitSnapToTerrainPlane(collinear));
    CHECK_FALSE(mir::FitSnapToTerrainPlane(std::span<const glm::dvec3>(points).first(2)));
}

TEST_CASE("The fit residual is the worst absolute error, not RMS", "[editor][snap]")
{
    std::vector<glm::dvec3> points{
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, {1.0, 0.0, 1.0},
        {2.0, 0.0, 0.0}, {2.0, 0.0, 1.0}, {0.0, 0.0, 2.0}, {1.0, 0.0, 2.0}, {2.0, 5.0, 2.0}};
    const std::optional<mir::SnapToTerrainPlane> plane = mir::FitSnapToTerrainPlane(points);
    REQUIRE(plane);

    double worst = 0.0;
    double squared = 0.0;
    for (const glm::dvec3 &point : points)
    {
        const double error = glm::dot(point - plane->mPoint, plane->mNormal);
        worst = std::max(worst, std::abs(error));
        squared += error * error;
    }
    const double rms = std::sqrt(squared / static_cast<double>(points.size()));
    CHECK(plane->mResidual == Catch::Approx(worst).epsilon(1e-12));
    CHECK(plane->mResidual > rms);
}

TEST_CASE("Snap rotation keeps the world-Y heading and drops old pitch and roll", "[editor][snap]")
{
    const glm::quat current = glm::angleAxis(0.7f, glm::vec3(0.0f, 1.0f, 0.0f)) *
                              glm::angleAxis(0.4f, glm::vec3(1.0f, 0.0f, 0.0f)) *
                              glm::angleAxis(-0.3f, glm::vec3(0.0f, 0.0f, 1.0f));
    const std::optional<glm::quat> flat = mir::SnapToTerrainWorldRotation(current, {0.0, 1.0, 0.0});
    REQUIRE(flat);
    const glm::quat expected = glm::angleAxis(0.7f, glm::vec3(0.0f, 1.0f, 0.0f));
    CHECK(std::abs(glm::dot(*flat, expected)) == Catch::Approx(1.0f).epsilon(1e-5));

    const glm::dvec3 normal = glm::normalize(glm::dvec3(-0.1, 1.0, -0.2));
    const std::optional<glm::quat> tilted = mir::SnapToTerrainWorldRotation(current, normal);
    REQUIRE(tilted);
    const glm::vec3 up = *tilted * glm::vec3(0.0f, 1.0f, 0.0f);
    CHECK(std::abs(glm::dot(glm::normalize(up), glm::vec3(normal))) == Catch::Approx(1.0f).epsilon(1e-5));

    CHECK_FALSE(mir::SnapToTerrainWorldRotation(current, {0.0, 0.0, 1.0}));
}

TEST_CASE("A parent must be uniform, unsheared and invertible", "[editor][snap]")
{
    const glm::quat rotation = glm::angleAxis(0.5f, glm::normalize(glm::vec3(1.0f, 2.0f, 3.0f)));
    const std::optional<glm::quat> uniform = mir::TrySnapParentRotation(glm::mat3_cast(rotation) * 2.0f);
    REQUIRE(uniform);
    CHECK(std::abs(glm::dot(*uniform, rotation)) == Catch::Approx(1.0f).epsilon(1e-5));

    const glm::mat3 nonuniform = glm::mat3(glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
                                           glm::vec3(0.0f, 0.0f, 1.0f));
    CHECK_FALSE(mir::TrySnapParentRotation(nonuniform));

    const glm::mat3 sheared = glm::mat3(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.2f, 1.0f, 0.0f),
                                        glm::vec3(0.0f, 0.0f, 1.0f));
    CHECK_FALSE(mir::TrySnapParentRotation(sheared));

    const glm::mat3 singular = glm::mat3(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 0.0f),
                                         glm::vec3(0.0f, 0.0f, 1.0f));
    CHECK_FALSE(mir::TrySnapParentRotation(singular));
}

TEST_CASE("A footprint uses corners, a grid or a cross by source spacing", "[editor][snap]")
{
    mir::SnapToTerrainFacts small;
    small.mHasBounds = true;
    small.mLocalMin = {-2.0, 0.0, -2.0};
    small.mLocalMax = {2.0, 1.0, 2.0};
    CHECK(mir::SnapToTerrainFootprint(At(3.0, 10.0, 4.0), glm::mat3(1.0f), small, 4.0).size() == 5);

    mir::SnapToTerrainFacts large = small;
    large.mLocalMin = {-20.0, 0.0, -20.0};
    large.mLocalMax = {20.0, 1.0, 20.0};
    CHECK(mir::SnapToTerrainFootprint(At(3.0, 10.0, 4.0), glm::mat3(1.0f), large, 4.0).size() == 9);

    mir::SnapToTerrainFacts none;
    const std::vector<glm::dvec3> cross = mir::SnapToTerrainFootprint(At(3.0, 10.0, 4.0), glm::mat3(1.0f), none, 4.0);
    REQUIRE(cross.size() == 5);
    CHECK(cross[0] == glm::dvec3(3.0, 10.0, 4.0));
    CHECK(cross[1] == glm::dvec3(7.0, 10.0, 4.0));
    CHECK(cross[2] == glm::dvec3(-1.0, 10.0, 4.0));
    CHECK(cross[3] == glm::dvec3(3.0, 10.0, 8.0));
    CHECK(cross[4] == glm::dvec3(3.0, 10.0, 0.0));
}

TEST_CASE("A ready snap writes nothing until its edit is taken", "[editor][snap]")
{
    Fixture fixture;
    fixture.TransformOf().SetPosition(Vec(3.0, 10.0, 4.0));
    fixture.TransformOf().SetRotation(glm::angleAxis(0.5f, glm::vec3(0.0f, 1.0f, 0.0f)) *
                                      glm::angleAxis(0.3f, glm::vec3(1.0f, 0.0f, 0.0f)));
    fixture.mSource.mC = 5.0;

    SnapToTerrainSession session;
    const SnapToTerrainAction action = session.Begin(fixture.mWorld, mir::kSceneDocument, *fixture.mGround);
    CHECK(action.mEnabled);
    REQUIRE(session.CurrentState() == SnapToTerrainSession::State::Ready);
    CHECK(fixture.TransformOf().Position() == Vec(3.0, 10.0, 4.0));

    const std::optional<SnapToTerrainEdit> edit = session.TakeEdit();
    REQUIRE(edit);
    CHECK(edit->mPosition.y == Catch::Approx(5.0).epsilon(1e-5));
    CHECK(edit->mPosition.x == Catch::Approx(3.0).epsilon(1e-5));
    CHECK(edit->mPosition.z == Catch::Approx(4.0).epsilon(1e-5));
    CHECK(edit->mSamples == 5);
    CHECK(edit->mResidual == Catch::Approx(0.0).margin(1e-6));
    CHECK(std::abs(glm::dot(edit->mRotation,
                            glm::angleAxis(0.5f, glm::vec3(0.0f, 1.0f, 0.0f)))) ==
          Catch::Approx(1.0f).epsilon(1e-5));
}

TEST_CASE("A loading snap retries without mutating and times out", "[editor][snap]")
{
    Fixture fixture;
    SnapToTerrainSession session;
    fixture.mSource.mLoading = true;
    REQUIRE(session.Begin(fixture.mWorld, mir::kSceneDocument, *fixture.mGround).mEnabled);
    CHECK(session.CurrentState() == SnapToTerrainSession::State::Waiting);

    for (int i = 0; i < 3; ++i)
        session.Update(fixture.mWorld, mir::kSceneDocument, *fixture.mGround, 1.0f, true);
    CHECK(session.CurrentState() == SnapToTerrainSession::State::Waiting);
    CHECK(fixture.TransformOf().Position() == WorldVec3{0.0});

    fixture.mSource.mLoading = false;
    session.Update(fixture.mWorld, mir::kSceneDocument, *fixture.mGround, 0.1f, true);
    CHECK(session.CurrentState() == SnapToTerrainSession::State::Ready);
    REQUIRE(session.TakeEdit());

    fixture.mSource.mLoading = true;
    REQUIRE(session.Begin(fixture.mWorld, mir::kSceneDocument, *fixture.mGround).mEnabled);
    session.Update(fixture.mWorld, mir::kSceneDocument, *fixture.mGround, 11.0f, true);
    CHECK(session.CurrentState() == SnapToTerrainSession::State::Failed);
    CHECK(session.TakeFailure().find("timed out") != std::string::npos);
}

TEST_CASE("A snap cancels on binding changes", "[editor][snap]")
{
    SECTION("selection")
    {
        Fixture fixture;
        fixture.mSource.mLoading = true;
        SnapToTerrainSession session;
        REQUIRE(session.Begin(fixture.mWorld, mir::kSceneDocument, *fixture.mGround).mEnabled);
        const Entity other = fixture.mWorld.CreateEntity();
        fixture.Select(other);
        session.Update(fixture.mWorld, mir::kSceneDocument, *fixture.mGround, 0.1f, true);
        CHECK(session.CurrentState() == SnapToTerrainSession::State::Failed);
    }
    SECTION("transform")
    {
        Fixture fixture;
        fixture.mSource.mLoading = true;
        SnapToTerrainSession session;
        REQUIRE(session.Begin(fixture.mWorld, mir::kSceneDocument, *fixture.mGround).mEnabled);
        fixture.TransformOf().SetRotation(glm::angleAxis(0.2f, glm::vec3(0.0f, 1.0f, 0.0f)));
        session.Update(fixture.mWorld, mir::kSceneDocument, *fixture.mGround, 0.1f, true);
        CHECK(session.CurrentState() == SnapToTerrainSession::State::Failed);
    }
    SECTION("terrain release")
    {
        Fixture fixture;
        fixture.mSource.mLoading = true;
        SnapToTerrainSession session;
        REQUIRE(session.Begin(fixture.mWorld, mir::kSceneDocument, *fixture.mGround).mEnabled);
        fixture.mGround->Invalidate();
        session.Update(fixture.mWorld, mir::kSceneDocument, *fixture.mGround, 0.1f, true);
        CHECK(session.CurrentState() == SnapToTerrainSession::State::Failed);
    }
    SECTION("play")
    {
        Fixture fixture;
        fixture.mSource.mLoading = true;
        SnapToTerrainSession session;
        REQUIRE(session.Begin(fixture.mWorld, mir::kSceneDocument, *fixture.mGround).mEnabled);
        session.Update(fixture.mWorld, mir::kSceneDocument, *fixture.mGround, 0.1f, false);
        CHECK(session.CurrentState() == SnapToTerrainSession::State::Failed);
    }
    SECTION("document")
    {
        Fixture fixture;
        fixture.mSource.mLoading = true;
        SnapToTerrainSession session;
        REQUIRE(session.Begin(fixture.mWorld, mir::kSceneDocument, *fixture.mGround).mEnabled);
        session.Update(fixture.mWorld, DocumentId{9, 9}, *fixture.mGround, 0.1f, true);
        CHECK(session.CurrentState() == SnapToTerrainSession::State::Failed);
    }
    SECTION("parent")
    {
        Fixture fixture;
        const Entity parent = fixture.mWorld.CreateEntity();
        mir::AddTransform(fixture.mWorld, parent, Transform(WorldVec3{0.0}));
        REQUIRE(mir::SetParent(fixture.mWorld, fixture.mEntity, parent));
        fixture.mSource.mLoading = true;
        SnapToTerrainSession session;
        REQUIRE(session.Begin(fixture.mWorld, mir::kSceneDocument, *fixture.mGround).mEnabled);
        fixture.mWorld.GetComponent<Transform>(parent)->SetPosition(Vec(1.0, 0.0, 0.0));
        session.Update(fixture.mWorld, mir::kSceneDocument, *fixture.mGround, 0.1f, true);
        CHECK(session.CurrentState() == SnapToTerrainSession::State::Failed);
    }
}

TEST_CASE("A uniform parent scale reaches the footprint and final contact", "[editor][snap]")
{
    Fixture fixture;
    MeshBounds *bounds = fixture.mWorld.GetComponent<MeshBounds>(fixture.mEntity);
    REQUIRE(bounds != nullptr);
    bounds->mLocalMin = glm::vec3(-4.0f, -1.0f, -1.0f);
    bounds->mLocalMax = glm::vec3(2.0f, 2.0f, 3.0f);
    fixture.mSource.mA = 0.1;
    fixture.mSource.mB = 0.2;

    const Entity parent = fixture.mWorld.CreateEntity();
    mir::AddTransform(fixture.mWorld, parent, Transform(Vec(10.0, 0.0, 5.0),
        glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(2.0f)));
    REQUIRE(mir::SetParent(fixture.mWorld, fixture.mEntity, parent));
    fixture.TransformOf().SetPosition(Vec(1.0, 2.0, 3.0));

    SnapToTerrainSession session;
    REQUIRE(session.Begin(fixture.mWorld, mir::kSceneDocument, *fixture.mGround).mEnabled);
    const std::optional<SnapToTerrainEdit> edit = session.TakeEdit();
    REQUIRE(edit);

    const mir::WorldPose parentPose = mir::ResolveWorld(fixture.mWorld, parent);
    const glm::vec3 localScale = fixture.TransformOf().Scale();
    glm::mat3 localLinear = glm::mat3_cast(edit->mRotation);
    localLinear[0] *= localScale.x;
    localLinear[1] *= localScale.y;
    localLinear[2] *= localScale.z;
    const std::optional<mir::WorldPose> worldPose = mir::TryComposePose(parentPose, localLinear, edit->mPosition);
    REQUIRE(worldPose);
    const glm::vec3 bottomCentre(-1.0f, -1.0f, 1.0f);
    const glm::dvec3 bottom = glm::dvec3(worldPose->position.Meters()) +
                              glm::dvec3(worldPose->linear * bottomCentre);
    const double expected = 5.0 + 0.1 * bottom.x + 0.2 * bottom.z;
    CHECK(bottom.y == Catch::Approx(expected).margin(0.05));
}

TEST_CASE("The second fitted normal also places an off-centre footprint", "[editor][snap]")
{
    Fixture fixture;
    MeshBounds *bounds = fixture.mWorld.GetComponent<MeshBounds>(fixture.mEntity);
    REQUIRE(bounds != nullptr);
    bounds->mLocalMin = glm::vec3(-4.0f, -1.0f, -1.0f);
    bounds->mLocalMax = glm::vec3(2.0f, 2.0f, 3.0f);
    fixture.mSource.mChangeAfterCalls = 5;
    fixture.mSource.mA2 = 0.2;
    fixture.mSource.mB2 = -0.1;

    SnapToTerrainSession session;
    REQUIRE(session.Begin(fixture.mWorld, mir::kSceneDocument, *fixture.mGround).mEnabled);
    const std::optional<SnapToTerrainEdit> edit = session.TakeEdit();
    REQUIRE(edit);

    const glm::dvec3 normal = glm::normalize(glm::dvec3(-0.2, 1.0, 0.1));
    const glm::vec3 up = edit->mRotation * glm::vec3(0.0f, 1.0f, 0.0f);
    CHECK(std::abs(glm::dot(glm::normalize(up), glm::vec3(normal))) == Catch::Approx(1.0f).epsilon(1e-5));

    const glm::vec3 bottomCentre(-1.0f, -1.0f, 1.0f);
    const glm::dvec3 bottom = glm::dvec3(edit->mPosition) +
                              glm::dvec3(glm::mat3_cast(edit->mRotation) * bottomCentre);
    const double expected = 5.0 + 0.2 * bottom.x - 0.1 * bottom.z;
    CHECK(bottom.y == Catch::Approx(expected).margin(0.05));
}

TEST_CASE("Snap availability names every disabled reason", "[editor][snap]")
{
    Fixture fixture;
    CHECK(mir::SnapToTerrainActionFor(fixture.mWorld, mir::kSceneDocument, fixture.mGround).mEnabled);

    const SnapToTerrainAction prefab =
        mir::SnapToTerrainActionFor(fixture.mWorld, DocumentId{1, 1}, fixture.mGround);
    CHECK_FALSE(prefab.mEnabled);
    CHECK(prefab.mReason.find("Prefab") != std::string::npos);

    fixture.mSource.mCovered = false;
    const SnapToTerrainAction uncovered =
        mir::SnapToTerrainActionFor(fixture.mWorld, mir::kSceneDocument, fixture.mGround);
    CHECK_FALSE(uncovered.mEnabled);
    CHECK(uncovered.mReason.find("covers") != std::string::npos);
    fixture.mSource.mCovered = true;

    fixture.mSource.mLoading = true;
    const SnapToTerrainAction waiting =
        mir::SnapToTerrainActionFor(fixture.mWorld, mir::kSceneDocument, fixture.mGround);
    CHECK(waiting.mEnabled);
    CHECK_FALSE(waiting.mStatus.empty());
    fixture.mSource.mLoading = false;

    const SnapToTerrainAction noGround = mir::SnapToTerrainActionFor(fixture.mWorld, mir::kSceneDocument, nullptr);
    CHECK_FALSE(noGround.mEnabled);
    CHECK(noGround.mReason.find("terrain") != std::string::npos);

    fixture.mWorld.Resource<Selection>().mSelected.clear();
    CHECK_FALSE(mir::SnapToTerrainActionFor(fixture.mWorld, mir::kSceneDocument, fixture.mGround).mEnabled);
    fixture.Select(fixture.mEntity);

    fixture.mWorld.Resource<EngineModeState>().mMode = EngineMode::Play;
    CHECK_FALSE(mir::SnapToTerrainActionFor(fixture.mWorld, mir::kSceneDocument, fixture.mGround).mEnabled);
    fixture.mWorld.Resource<EngineModeState>().mMode = EngineMode::Edit;

    const Entity parent = fixture.mWorld.CreateEntity();
    mir::AddTransform(fixture.mWorld, parent, Transform(WorldVec3{0.0}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                                                          glm::vec3(2.0f, 1.0f, 1.0f)));
    REQUIRE(mir::SetParent(fixture.mWorld, fixture.mEntity, parent));
    const SnapToTerrainAction badParent =
        mir::SnapToTerrainActionFor(fixture.mWorld, mir::kSceneDocument, fixture.mGround);
    CHECK_FALSE(badParent.mEnabled);
    CHECK(badParent.mReason.find("parent") != std::string::npos);
}

TEST_CASE("One snap command restores pose and scale through ordinary undo and redo", "[editor][snap]")
{
    Editing editing;
    const Entity entity = editing.Add("snap");
    Transform original(WorldVec3{1.0});
    original.SetRotation(glm::angleAxis(0.2f, glm::vec3(0.0f, 1.0f, 0.0f)));
    original.SetScale(glm::vec3(2.0f, 3.0f, 4.0f));
    mir::AddTransform(editing.mWorld, entity, original);

    SnapToTerrainEdit edit;
    edit.mPosition = Vec(10.0, 20.0, 30.0);
    edit.mRotation = glm::angleAxis(0.6f, glm::vec3(0.0f, 1.0f, 0.0f));
    std::unique_ptr<mir::ICommand> command = mir::MakeSnapToTerrainCommand(entity, edit, nullptr);
    REQUIRE(command);
    editing.Run(std::move(command));
    CHECK(editing.mHistory.StepCount() == 1);
    CHECK(editing.mHistory.UndoName() == "Snap to Terrain");

    const Transform *transform = editing.mWorld.GetComponent<Transform>(entity);
    CHECK(transform->Position() == edit.mPosition);
    CHECK(std::abs(glm::dot(transform->Rotation(), edit.mRotation)) == Catch::Approx(1.0f).epsilon(1e-5));
    CHECK(transform->Scale() == glm::vec3(2.0f, 3.0f, 4.0f));

    editing.Undo();
    transform = editing.mWorld.GetComponent<Transform>(entity);
    CHECK(transform->Position() == original.Position());
    CHECK(std::abs(glm::dot(transform->Rotation(), original.Rotation())) == Catch::Approx(1.0f).epsilon(1e-5));
    CHECK(transform->Scale() == original.Scale());

    editing.Redo();
    transform = editing.mWorld.GetComponent<Transform>(entity);
    CHECK(transform->Position() == edit.mPosition);
    CHECK(transform->Scale() == original.Scale());
}

TEST_CASE("A member snap writes both Transform fields through the authored backend", "[editor][snap]")
{
    Editing editing;
    const Entity entity = editing.Add("member");
    mir::AddTransform(editing.mWorld, entity, Transform(WorldVec3{1.0}));
    FakeBackend backend;
    FakeView view;
    view.mMember = true;
    view.mTarget = entity;
    editing.mHistory.SetAuthoringBackend(&backend);

    SnapToTerrainEdit edit;
    edit.mPosition = Vec(10.0, 20.0, 30.0);
    edit.mRotation = glm::angleAxis(0.6f, glm::vec3(0.0f, 1.0f, 0.0f));
    std::unique_ptr<mir::ICommand> command = mir::MakeSnapToTerrainCommand(entity, edit, &view);
    REQUIRE(command);
    editing.Run(std::move(command));

    CHECK(editing.mHistory.StepCount() == 1);
    REQUIRE(backend.mWritten.size() == 2);
    CHECK(backend.mWritten[0].mComponent == "Transform");
    CHECK(backend.mWritten[1].mComponent == "Transform");
    CHECK(backend.mWritten[0].mField != backend.mWritten[1].mField);
    CHECK(backend.At(backend.mWritten[0]));
    CHECK(backend.At(backend.mWritten[1]));

    editing.Undo();
    CHECK_FALSE(backend.At(backend.mWritten[0]));
    CHECK_FALSE(backend.At(backend.mWritten[1]));

    editing.Redo();
    CHECK(backend.At(backend.mWritten[0]));
    CHECK(backend.At(backend.mWritten[1]));
}
