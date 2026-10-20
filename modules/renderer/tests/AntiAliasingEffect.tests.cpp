/**
 * @file AntiAliasingEffect.tests.cpp
 * @author Rahul Nair
 * @brief Tests for renderer/AntiAliasingEffect.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/AntiAliasingEffect.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

TEST_CASE("AntiAliasingEffect: ReadAntiAliasingParams on an empty params object returns defaults", "[renderer]")
{
    const nlohmann::json params = nlohmann::json::object();
    const mir::AntiAliasingParams aa = mir::ReadAntiAliasingParams(params);

    CHECK(aa.mode == mir::AntiAliasingParams{}.mode);
    CHECK(aa.rtSampleCount == mir::AntiAliasingParams{}.rtSampleCount);
}

TEST_CASE("AntiAliasingEffect: ReadAntiAliasingParams falls back per field when some are missing", "[renderer]")
{
    const nlohmann::json params = { { "rtSampleCount", 2 } };
    const mir::AntiAliasingParams aa = mir::ReadAntiAliasingParams(params);

    CHECK(aa.mode == mir::AntiAliasingParams{}.mode);
    CHECK(aa.rtSampleCount == 2);
}

TEST_CASE("AntiAliasingEffect: WriteAntiAliasingParams then ReadAntiAliasingParams round-trips every field",
    "[renderer]")
{
    mir::AntiAliasingParams saved;
    saved.mode = mir::AntiAliasingMode::Msaa;
    saved.rtSampleCount = 2;

    nlohmann::json params = nlohmann::json::object();
    mir::WriteAntiAliasingParams(params, saved);

    const mir::AntiAliasingParams loaded = mir::ReadAntiAliasingParams(params);
    CHECK(loaded.mode == saved.mode);
    CHECK(loaded.rtSampleCount == saved.rtSampleCount);
}

TEST_CASE("AntiAliasingEffect: ToString/ParseMode round-trip every mode", "[renderer]")
{
    CHECK(mir::ParseMode(mir::ToString(mir::AntiAliasingMode::Off)) == mir::AntiAliasingMode::Off);
    CHECK(mir::ParseMode(mir::ToString(mir::AntiAliasingMode::Fxaa)) == mir::AntiAliasingMode::Fxaa);
    CHECK(mir::ParseMode(mir::ToString(mir::AntiAliasingMode::Msaa)) == mir::AntiAliasingMode::Msaa);
    CHECK(mir::ParseMode(mir::ToString(mir::AntiAliasingMode::Dlss)) == mir::AntiAliasingMode::Dlss);
}

TEST_CASE("AntiAliasingEffect: ParseMode falls back to Off on an unrecognized name", "[renderer]")
{
    CHECK(mir::ParseMode("not-a-real-mode") == mir::AntiAliasingMode::Off);
    CHECK(mir::ParseMode("") == mir::AntiAliasingMode::Off);
}

TEST_CASE("AntiAliasingEffect: ClampAntiAliasingMode allows Off regardless of pipeline/hardware", "[renderer]")
{
    CHECK(mir::ClampAntiAliasingMode(mir::AntiAliasingMode::Off, false, false) == mir::AntiAliasingMode::Off);
    CHECK(mir::ClampAntiAliasingMode(mir::AntiAliasingMode::Off, true, true) == mir::AntiAliasingMode::Off);
}

TEST_CASE("AntiAliasingEffect: ClampAntiAliasingMode allows Fxaa only on raster", "[renderer]")
{
    CHECK(mir::ClampAntiAliasingMode(mir::AntiAliasingMode::Fxaa, false, false) == mir::AntiAliasingMode::Fxaa);
    CHECK(mir::ClampAntiAliasingMode(mir::AntiAliasingMode::Fxaa, true, false) == mir::AntiAliasingMode::Off);
}

TEST_CASE("AntiAliasingEffect: ClampAntiAliasingMode allows Msaa only on ray tracing", "[renderer]")
{
    CHECK(mir::ClampAntiAliasingMode(mir::AntiAliasingMode::Msaa, true, false) == mir::AntiAliasingMode::Msaa);
    CHECK(mir::ClampAntiAliasingMode(mir::AntiAliasingMode::Msaa, false, false) == mir::AntiAliasingMode::Off);
}

TEST_CASE("AntiAliasingEffect: ClampAntiAliasingMode allows Dlss only on ray tracing with NGX support",
    "[renderer]")
{
    CHECK(mir::ClampAntiAliasingMode(mir::AntiAliasingMode::Dlss, true, true) == mir::AntiAliasingMode::Dlss);
    CHECK(mir::ClampAntiAliasingMode(mir::AntiAliasingMode::Dlss, true, false) == mir::AntiAliasingMode::Off);
    CHECK(mir::ClampAntiAliasingMode(mir::AntiAliasingMode::Dlss, false, true) == mir::AntiAliasingMode::Off);
}
