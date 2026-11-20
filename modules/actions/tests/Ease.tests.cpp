/**
 * @file Ease.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests the easing curves' end points, shapes and names.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <actions/Ease.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace mir;

namespace
{
    constexpr std::array kEveryEase{ Ease::Linear, Ease::In, Ease::Out, Ease::InOut,
                                     Ease::StrongIn, Ease::StrongOut, Ease::Back, Ease::Bounce };
}

TEST_CASE("Every ease starts at 0, ends at 1 and clamps outside the range", "[actions][ease]")
{
    for (const Ease ease : kEveryEase)
    {
        INFO(EaseName(ease));
        CHECK(ApplyEase(ease, 0.0f) == 0.0f);
        CHECK(ApplyEase(ease, 1.0f) == 1.0f);
        CHECK(ApplyEase(ease, -3.0f) == 0.0f);
        CHECK(ApplyEase(ease, 4.0f) == 1.0f);
    }
}

TEST_CASE("In starts slow and Out ends slow", "[actions][ease]")
{
    CHECK(ApplyEase(Ease::In, 0.5f) < 0.5f);
    CHECK(ApplyEase(Ease::Out, 0.5f) > 0.5f);
    CHECK(ApplyEase(Ease::StrongIn, 0.5f) < ApplyEase(Ease::In, 0.5f));
    CHECK(ApplyEase(Ease::StrongOut, 0.5f) > ApplyEase(Ease::Out, 0.5f));
    CHECK(ApplyEase(Ease::InOut, 0.5f) == Catch::Approx(0.5f));
    CHECK(ApplyEase(Ease::Linear, 0.25f) == 0.25f);
}

TEST_CASE("Back overshoots before settling", "[actions][ease]")
{
    CHECK(ApplyEase(Ease::Back, 0.8f) > 1.0f);
}

TEST_CASE("Ease names round trip, and an unknown name is refused", "[actions][ease]")
{
    for (const Ease ease : kEveryEase)
        CHECK(EaseFromName(EaseName(ease)) == ease);
    CHECK_FALSE(EaseFromName("easeIn").has_value());
    CHECK_FALSE(EaseFromName("").has_value());
}
