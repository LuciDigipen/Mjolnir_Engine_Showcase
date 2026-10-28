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

    CHECK(dof.maxBlur == mir::DepthOfFieldParams{}.maxBlur);
}

TEST_CASE("DepthOfFieldEffect: focus distance and aperture are the Camera's, not part of the params", "[renderer]")
{
    nlohmann::json written = nlohmann::json::object();
    mir::WriteDepthOfFieldParams(written, mir::DepthOfFieldParams{});

    CHECK_FALSE(written.contains("focusDistance"));
    CHECK_FALSE(written.contains("aperture"));
}

TEST_CASE("DepthOfFieldEffect: WriteDepthOfFieldParams then ReadDepthOfFieldParams round-trips every field",
    "[renderer]")
{
    mir::DepthOfFieldParams saved;
    saved.maxBlur = 32.0f;

    nlohmann::json params = nlohmann::json::object();
    mir::WriteDepthOfFieldParams(params, saved);

    CHECK(mir::ReadDepthOfFieldParams(params).maxBlur == saved.maxBlur);
}
