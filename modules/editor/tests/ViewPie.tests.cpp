#include <editor/ViewPie.h>

#include <catch2/catch_test_macros.hpp>

using namespace mir;

TEST_CASE("View pie picks the wedge the mouse points into", "[editor][view-pie]")
{
    constexpr float kDeadZone = 20.0f;

    CHECK(ViewPieItemAt({ 50.0f, 0.0f }, kDeadZone) == ViewPieItem::Render);
    CHECK(ViewPieItemAt({ -50.0f, 0.0f }, kDeadZone) == ViewPieItem::Wireframe);
    // screen y grows down
    CHECK(ViewPieItemAt({ 0.0f, -50.0f }, kDeadZone) == ViewPieItem::RenderWireframe);
    CHECK(ViewPieItemAt({ 0.0f, 50.0f }, kDeadZone) == ViewPieItem::Colliders);

    // off-axis points go to the nearer axis, and an exact diagonal to the horizontal wedge
    CHECK(ViewPieItemAt({ 40.0f, -30.0f }, kDeadZone) == ViewPieItem::Render);
    CHECK(ViewPieItemAt({ -30.0f, 40.0f }, kDeadZone) == ViewPieItem::Colliders);
    CHECK(ViewPieItemAt({ -30.0f, -30.0f }, kDeadZone) == ViewPieItem::Wireframe);
}

TEST_CASE("View pie picks nothing inside the dead zone", "[editor][view-pie]")
{
    constexpr float kDeadZone = 20.0f;

    CHECK_FALSE(ViewPieItemAt({ 0.0f, 0.0f }, kDeadZone).has_value());
    CHECK_FALSE(ViewPieItemAt({ 20.0f, 0.0f }, kDeadZone).has_value());
    CHECK_FALSE(ViewPieItemAt({ 12.0f, -12.0f }, kDeadZone).has_value());
    CHECK(ViewPieItemAt({ 20.5f, 0.0f }, kDeadZone) == ViewPieItem::Render);
}

TEST_CASE("Each view pie item's direction lands in its own wedge", "[editor][view-pie]")
{
    for (const ViewPieItem item : kViewPieItems)
        CHECK(ViewPieItemAt(ViewPieDirection(item) * 100.0f, 20.0f) == item);
}

TEST_CASE("A shading item replaces the shading and keeps colliders", "[editor][view-pie]")
{
    DebugViewSettings view;
    view.colliders = true;

    ApplyViewPieItem(view, ViewPieItem::Wireframe);
    CHECK(view.shading == ViewportShading::Wireframe);
    CHECK(view.colliders);
    CHECK(ViewPieItemActive(view, ViewPieItem::Wireframe));
    CHECK_FALSE(ViewPieItemActive(view, ViewPieItem::Render));

    ApplyViewPieItem(view, ViewPieItem::RenderWireframe);
    CHECK(view.shading == ViewportShading::RenderWireframe);

    ApplyViewPieItem(view, ViewPieItem::Render);
    CHECK(view.shading == ViewportShading::Render);
    CHECK(view.colliders);
}

TEST_CASE("Colliders toggles over the current shading", "[editor][view-pie]")
{
    DebugViewSettings view;
    view.shading = ViewportShading::Wireframe;

    ApplyViewPieItem(view, ViewPieItem::Colliders);
    CHECK(view.colliders);
    CHECK(view.shading == ViewportShading::Wireframe);
    CHECK(ViewPieItemActive(view, ViewPieItem::Colliders));

    ApplyViewPieItem(view, ViewPieItem::Colliders);
    CHECK_FALSE(view.colliders);
    CHECK_FALSE(ViewPieItemActive(view, ViewPieItem::Colliders));
}

TEST_CASE("Only the plain final render counts as no debug view", "[editor][view-pie]")
{
    DebugViewSettings view;
    CHECK_FALSE(view.Active());

    view.colliders = true;
    CHECK(view.Active());

    view.colliders = false;
    view.shading = ViewportShading::RenderWireframe;
    CHECK(view.Active());
}
