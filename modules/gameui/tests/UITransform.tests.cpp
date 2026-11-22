/**
 * @file UITransform.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for gameui/UITransform: moves, turns about a pivot, scales,
 *        composition order, the inverse and transformed bounds.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UITransform.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numbers>
#include <optional>

using namespace mir;

namespace
{
    constexpr float kQuarterTurn = std::numbers::pi_v<float> * 0.5f;

    void CheckPoint(UIPoint actual, UIPoint expected)
    {
        CHECK(actual.x == Catch::Approx(expected.x).margin(1e-4));
        CHECK(actual.y == Catch::Approx(expected.y).margin(1e-4));
    }
} // namespace

TEST_CASE("A transform moves, turns clockwise on screen about its pivot, and scales about its pivot", "[gameui][transform]")
{
    CheckPoint(UITransform::Translate(5, -2).Apply({1, 1}), {6, -1});

    // y is down, so a quarter turn takes right to down
    CheckPoint(UITransform::Rotate(kQuarterTurn).Apply({1, 0}), {0, 1});
    CheckPoint(UITransform::Rotate(kQuarterTurn, {10, 10}).Apply({11, 10}), {10, 11});
    CheckPoint(UITransform::Rotate(kQuarterTurn, {10, 10}).Apply({10, 10}), {10, 10});

    CheckPoint(UITransform::Scale(2, 3, {10, 10}).Apply({11, 11}), {12, 13});
    CHECK(UITransform::Scale(2, 2).AreaScale() == Catch::Approx(2.0f));
    CHECK(UITransform::Rotate(1.0f).AreaScale() == Catch::Approx(1.0f));
    CHECK(UITransform{}.IsIdentity());
    CHECK_FALSE(UITransform::Translate(1, 0).IsIdentity());
}

TEST_CASE("Composed transforms apply the inner one first", "[gameui][transform]")
{
    const UITransform moveThenTurn = UITransform::Rotate(kQuarterTurn) * UITransform::Translate(10, 0);
    CheckPoint(moveThenTurn.Apply({0, 0}), {0, 10});

    const UITransform turnThenMove = UITransform::Translate(10, 0) * UITransform::Rotate(kQuarterTurn);
    CheckPoint(turnThenMove.Apply({0, 0}), {10, 0});
    CheckPoint(turnThenMove.Apply({1, 0}), {10, 1});
}

TEST_CASE("The inverse undoes a transform, and a flattening one has none", "[gameui][transform]")
{
    const UITransform transform =
        UITransform::Translate(30, -4) * UITransform::Rotate(0.7f, {5, 5}) * UITransform::Scale(2, 0.5f);
    const std::optional<UITransform> inverse = transform.Inverse();
    REQUIRE(inverse.has_value());
    CheckPoint(inverse->Apply(transform.Apply({12, -7})), {12, -7});
    CheckPoint(transform.Apply(inverse->Apply({3, 9})), {3, 9});

    CHECK_FALSE(UITransform::Scale(0, 1).Inverse().has_value());
}

TEST_CASE("Bounds is the axis-aligned box round a transformed rect", "[gameui][transform]")
{
    const UIRect rect{0, 0, 20, 10};
    CHECK(UITransform{}.Bounds(rect) == rect);

    const UIRect moved = UITransform::Translate(5, 5).Bounds(rect);
    CHECK(moved.x == Catch::Approx(5));
    CHECK(moved.width == Catch::Approx(20));

    // turned a quarter about its centre, it is 10 wide and 20 tall
    const UIRect turned = UITransform::Rotate(kQuarterTurn, {10, 5}).Bounds(rect);
    CHECK(turned.x == Catch::Approx(5).margin(1e-4));
    CHECK(turned.y == Catch::Approx(-5).margin(1e-4));
    CHECK(turned.width == Catch::Approx(10).margin(1e-4));
    CHECK(turned.height == Catch::Approx(20).margin(1e-4));
}
