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
    CHECK(aa.dlssRrPreset == mir::DlssRrPreset::E);
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
    saved.dlssRrPreset = mir::DlssRrPreset::D;
    saved.dlssRayReconstruction = false;

    nlohmann::json params = nlohmann::json::object();
    mir::WriteAntiAliasingParams(params, saved);

    const mir::AntiAliasingParams loaded = mir::ReadAntiAliasingParams(params);
    CHECK(loaded.mode == saved.mode);
    CHECK(loaded.rtSampleCount == saved.rtSampleCount);
    CHECK(loaded.dlssRrPreset == saved.dlssRrPreset);
    CHECK(loaded.dlssRayReconstruction == saved.dlssRayReconstruction);
}

TEST_CASE("AntiAliasingEffect: legacy DLSS settings select E and retain explicit comparison presets", "[renderer]")
{
    const auto legacy = mir::ReadAntiAliasingParams({{"mode", "dlss"}, {"rtSampleCount", 4}});
    CHECK(legacy.mode == mir::AntiAliasingMode::Dlss);
    CHECK(legacy.dlssRrPreset == mir::DlssRrPreset::E);
    CHECK(legacy.dlssRayReconstruction);
    for (auto preset : {mir::DlssRrPreset::D, mir::DlssRrPreset::E})
    {
        auto saved = legacy;
        saved.dlssRrPreset = preset;
        nlohmann::json params = { {"unrelated", 17} };
        mir::WriteAntiAliasingParams(params, saved);
        CHECK(mir::ReadAntiAliasingParams(params) == saved);
        CHECK(params["unrelated"] == 17);
    }
}

TEST_CASE("AntiAliasingEffect: unsupported or malformed RR presets safely select E", "[renderer]")
{
    for (const nlohmann::json &value : { nlohmann::json("f"), nlohmann::json("default"),
        nlohmann::json(""), nlohmann::json(6), nlohmann::json(nullptr), nlohmann::json(false) })
    {
        const auto loaded = mir::ReadAntiAliasingParams({{"mode", "dlss"}, {"dlssRrPreset", value}});
        CHECK(loaded.mode == mir::AntiAliasingMode::Dlss);
        CHECK(loaded.dlssRrPreset == mir::DlssRrPreset::E);
    }
}

TEST_CASE("AntiAliasingEffect: malformed RR switches retain the legacy enabled default", "[renderer]")
{
    for (const nlohmann::json &value : { nlohmann::json("false"), nlohmann::json(0), nlohmann::json(nullptr) })
        CHECK(mir::ReadAntiAliasingParams({{"dlssRayReconstruction", value}}).dlssRayReconstruction);
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

TEST_CASE("AntiAliasingEffect: ClampRtSampleCount rounds up to a standard sample pattern size", "[renderer]")
{
    CHECK(mir::ClampRtSampleCount(-3) == 1);
    CHECK(mir::ClampRtSampleCount(0) == 1);
    CHECK(mir::ClampRtSampleCount(1) == 1);
    CHECK(mir::ClampRtSampleCount(2) == 2);
    CHECK(mir::ClampRtSampleCount(3) == 4);
    CHECK(mir::ClampRtSampleCount(4) == 4);
    CHECK(mir::ClampRtSampleCount(5) == 8);
    CHECK(mir::ClampRtSampleCount(8) == 8);
    CHECK(mir::ClampRtSampleCount(64) == 8);
}
