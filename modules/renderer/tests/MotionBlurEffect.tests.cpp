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

    CHECK(motionBlur.shutterStrength == mir::MotionBlurParams{}.shutterStrength);
    CHECK(motionBlur.sampleCount == mir::MotionBlurParams{}.sampleCount);
}

TEST_CASE("MotionBlurEffect: ReadMotionBlurParams falls back per field when some are missing", "[renderer]")
{
    const nlohmann::json params = { { "sampleCount", 16 } };
    const mir::MotionBlurParams motionBlur = mir::ReadMotionBlurParams(params);

    CHECK(motionBlur.shutterStrength == mir::MotionBlurParams{}.shutterStrength);
    CHECK(motionBlur.sampleCount == 16);
}

TEST_CASE("MotionBlurEffect: WriteMotionBlurParams then ReadMotionBlurParams round-trips every field",
    "[renderer]")
{
    mir::MotionBlurParams saved;
    saved.shutterStrength = 1.25f;
    saved.sampleCount = 12;

    nlohmann::json params = nlohmann::json::object();
    mir::WriteMotionBlurParams(params, saved);

    const mir::MotionBlurParams loaded = mir::ReadMotionBlurParams(params);
    CHECK(loaded.shutterStrength == saved.shutterStrength);
    CHECK(loaded.sampleCount == saved.sampleCount);
}
