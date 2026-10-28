/**
 * @file MotionBlurEffect.tests.cpp
 * @author Rahul Nair
 * @brief Tests for renderer/MotionBlurEffect.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/MotionBlurEffect.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

TEST_CASE("MotionBlurEffect: ReadMotionBlurParams on an empty params object returns defaults", "[renderer]")
{
    const nlohmann::json params = nlohmann::json::object();
    const mir::MotionBlurParams motionBlur = mir::ReadMotionBlurParams(params);

    CHECK(motionBlur.sampleCount == mir::MotionBlurParams{}.sampleCount);
}

TEST_CASE("MotionBlurEffect: shutter strength is the Camera's, not part of the params", "[renderer]")
{
    nlohmann::json written = nlohmann::json::object();
    mir::WriteMotionBlurParams(written, mir::MotionBlurParams{});

    CHECK_FALSE(written.contains("shutterStrength"));
}

TEST_CASE("MotionBlurEffect: WriteMotionBlurParams then ReadMotionBlurParams round-trips every field",
    "[renderer]")
{
    mir::MotionBlurParams saved;
    saved.sampleCount = 12;

    nlohmann::json params = nlohmann::json::object();
    mir::WriteMotionBlurParams(params, saved);

    CHECK(mir::ReadMotionBlurParams(params).sampleCount == saved.sampleCount);
}
