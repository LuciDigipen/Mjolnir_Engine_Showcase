/**
 * @file DepthOfFieldEffect.tests.cpp
 * @author Rahul Nair
 * @brief Tests for renderer/DepthOfFieldEffect.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/DepthOfFieldEffect.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

TEST_CASE("DepthOfFieldEffect: ReadDepthOfFieldParams on an empty params object returns defaults", "[renderer]")
{
    const nlohmann::json params = nlohmann::json::object();
    const mir::DepthOfFieldParams dof = mir::ReadDepthOfFieldParams(params);

    CHECK(dof.focusDistance == mir::DepthOfFieldParams{}.focusDistance);
    CHECK(dof.aperture == mir::DepthOfFieldParams{}.aperture);
    CHECK(dof.maxBlur == mir::DepthOfFieldParams{}.maxBlur);
}

TEST_CASE("DepthOfFieldEffect: ReadDepthOfFieldParams falls back per field when some are missing", "[renderer]")
{
    const nlohmann::json params = { { "aperture", 0.25f } };
    const mir::DepthOfFieldParams dof = mir::ReadDepthOfFieldParams(params);

    CHECK(dof.focusDistance == mir::DepthOfFieldParams{}.focusDistance);
    CHECK(dof.aperture == 0.25f);
    CHECK(dof.maxBlur == mir::DepthOfFieldParams{}.maxBlur);
}

TEST_CASE("DepthOfFieldEffect: WriteDepthOfFieldParams then ReadDepthOfFieldParams round-trips every field",
    "[renderer]")
{
    mir::DepthOfFieldParams saved;
    saved.focusDistance = 25.0f;
    saved.aperture = 0.4f;
    saved.maxBlur = 32.0f;

    nlohmann::json params = nlohmann::json::object();
    mir::WriteDepthOfFieldParams(params, saved);

    const mir::DepthOfFieldParams loaded = mir::ReadDepthOfFieldParams(params);
    CHECK(loaded.focusDistance == saved.focusDistance);
    CHECK(loaded.aperture == saved.aperture);
    CHECK(loaded.maxBlur == saved.maxBlur);
}
