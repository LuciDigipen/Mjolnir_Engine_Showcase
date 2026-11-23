/**
 * @file UIWorldProjection.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests projecting world points into the game UI through a PrimaryView, and placing off-screen ones on the
 *        edge.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIContext.h>
#include <gameui/UILayout.h>
#include <gameui/UIWorldProjection.h>

#include <input/InputMap.h>
#include <input/InputState.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace mir;

namespace
{
    // the renderer's projection, for a camera at @p camera looking down -Z: y down, reverse-Z, w the depth
    PrimaryView LookingDownZ(WorldVec3 camera, float fovYDegrees, float aspect)
    {
        const float focal = 1.0f / std::tan(fovYDegrees * std::numbers::pi_v<float> / 360.0f);
        glm::mat4 projection{ 0.0f };
        projection[0][0] = focal / aspect;
        projection[1][1] = -focal;
        projection[2][3] = -1.0f;
        projection[3][2] = 0.1f;
        return PrimaryView{ *WorldPosition::FromMeters(camera), projection, 1280, 720, true };
    }

    WorldPosition At(WorldReal x, WorldReal y, WorldReal z) { return *WorldPosition::FromMeters(WorldVec3(x, y, z)); }

    constexpr UIRect kBounds{ 0, 0, 640, 360 };
    constexpr float kAspect = 640.0f / 360.0f;
}

TEST_CASE("A point straight ahead lands on the centre, its distance as the depth", "[gameui][world]")
{
    const PrimaryView view = LookingDownZ(WorldVec3(0), 90.0f, kAspect);
    const auto point = ProjectToUI(view, At(0, 0, -10), kBounds);
    REQUIRE(point.has_value());
    CHECK(point->mPoint.x == Catch::Approx(320.0f));
    CHECK(point->mPoint.y == Catch::Approx(180.0f));
    CHECK(point->mDepth == Catch::Approx(10.0f));
    CHECK(point->mInFront);
    CHECK(point->mOnScreen);
}

TEST_CASE("The edges of the field of view land on the edges of the bounds, up at the top", "[gameui][world]")
{
    const PrimaryView view = LookingDownZ(WorldVec3(0), 90.0f, kAspect);

    CHECK(ProjectToUI(view, At(0, 10, -10), kBounds)->mPoint.y == Catch::Approx(0.0f).margin(1e-3));
    CHECK(ProjectToUI(view, At(0, -10, -10), kBounds)->mPoint.y == Catch::Approx(360.0f));
    // the aspect widens the view, not the bounds
    CHECK(ProjectToUI(view, At(10 * kAspect, 0, -10), kBounds)->mPoint.x == Catch::Approx(640.0f));
    CHECK(ProjectToUI(view, At(-10 * kAspect, 0, -10), kBounds)->mPoint.x == Catch::Approx(0.0f).margin(1e-3));

    const auto outside = ProjectToUI(view, At(30, 0, -10), kBounds);
    CHECK(outside->mInFront);
    CHECK_FALSE(outside->mOnScreen);
}

TEST_CASE("A point behind the camera is not in front or on screen, and keeps its side", "[gameui][world]")
{
    const PrimaryView view = LookingDownZ(WorldVec3(0), 90.0f, kAspect);
    const auto point = ProjectToUI(view, At(5, 0, 10), kBounds);
    REQUIRE(point.has_value());
    CHECK_FALSE(point->mInFront);
    CHECK_FALSE(point->mOnScreen);
    CHECK(point->mDepth == Catch::Approx(-10.0f));
    CHECK(point->mPoint.x > 320.0f);

    // on the camera plane it still has a side, and no infinity
    const auto beside = ProjectToUI(view, At(-5, 0, 0), kBounds);
    REQUIRE(beside.has_value());
    CHECK(std::isfinite(beside->mPoint.x));
    CHECK(beside->mPoint.x < 0.0f);
}

TEST_CASE("Far from the origin a point is projected relative to the camera, so it stays exact", "[gameui][world]")
{
    const PrimaryView view = LookingDownZ(WorldVec3(100000, 0, 100000), 90.0f, kAspect);
    const auto point = ProjectToUI(view, At(100001, 0, 99990), kBounds);
    REQUIRE(point.has_value());
    // 1 m right at 10 m: ndc 0.1 / aspect
    CHECK(point->mPoint.x == Catch::Approx(320.0f + 0.1f / kAspect * 320.0f).margin(0.01));
    CHECK(point->mPoint.y == Catch::Approx(180.0f).margin(0.01));
}

TEST_CASE("Without a view there is no point", "[gameui][world]")
{
    CHECK_FALSE(ProjectToUI(PrimaryView{}, At(0, 0, -10), kBounds).has_value());
}

TEST_CASE("An edge point stays on screen, and goes to the edge toward a target off it or behind", "[gameui][world]")
{
    const UIRect edge = Inset(kBounds, 20.0f);

    const UIEdgePoint inside = EdgePoint(UIWorldPoint{ { 400, 100 }, 10, true, true }, kBounds, edge);
    CHECK(inside.mPoint == UIPoint{ 400, 100 });
    CHECK(inside.mAngle == Catch::Approx(std::atan2(-80.0f, 80.0f)));

    const UIEdgePoint right = EdgePoint(UIWorldPoint{ { 1000, 180 }, 10, true, false }, kBounds, edge);
    CHECK(right.mPoint.x == Catch::Approx(620.0f));
    CHECK(right.mPoint.y == Catch::Approx(180.0f));
    CHECK(right.mAngle == Catch::Approx(0.0f));

    // in the margin, on screen but past the edge
    const UIEdgePoint margin = EdgePoint(UIWorldPoint{ { 630, 180 }, 10, true, true }, kBounds, edge);
    CHECK(margin.mPoint.x == Catch::Approx(620.0f));

    // toward a corner: the nearer border, along the line
    const UIEdgePoint corner = EdgePoint(UIWorldPoint{ { 920, 540 }, 10, true, false }, kBounds, edge);
    CHECK(corner.mPoint.y == Catch::Approx(340.0f));
    CHECK(corner.mPoint.x == Catch::Approx(320.0f + 600.0f * (160.0f / 360.0f)));

    // behind, even when it projects inside, goes to the edge
    const UIEdgePoint behind = EdgePoint(UIWorldPoint{ { 200, 180 }, -10, false, false }, kBounds, edge);
    CHECK(behind.mPoint.x == Catch::Approx(20.0f));
    CHECK(behind.mAngle == Catch::Approx(std::numbers::pi_v<float>));

    // dead behind has no side, so it points down
    const UIEdgePoint dead = EdgePoint(UIWorldPoint{ { 320, 180 }, -10, false, false }, kBounds, edge);
    CHECK(dead.mPoint == UIPoint{ 320, 340 });
    CHECK(dead.mAngle == Catch::Approx(std::numbers::pi_v<float> * 0.5f));
}

TEST_CASE("UIContext projects into its bounds and puts edge points on its safe area", "[gameui][world]")
{
    InputState input;
    const InputMap map;
    UIContext ui;
    ui.SetView(LookingDownZ(WorldVec3(0), 90.0f, kAspect));
    ui.SetSafeAreaInsets(UIEdges{ 10, 10, 10, 10 });
    // the area's offset is the backend's, and the scale halves every unit
    ui.SetDesignSize(320.0f, 180.0f);
    ui.SetReferenceHeight(180.0f);
    ui.BeginFrame(UIRect{ 100, 50, 640, 360 }, input, map, UIPointer{});

    REQUIRE(ui.Scale() == Catch::Approx(2.0f));
    const auto ahead = ui.ProjectToUI(At(0, 0, -10));
    REQUIRE(ahead.has_value());
    CHECK(ahead->mPoint.x == Catch::Approx(160.0f));
    CHECK(ahead->mPoint.y == Catch::Approx(90.0f));

    const UIEdgePoint right = ui.EdgePoint(*ui.ProjectToUI(At(100, 0, -10)), 5.0f);
    CHECK(right.mPoint.x == Catch::Approx(305.0f));
    ui.EndFrame();
}
