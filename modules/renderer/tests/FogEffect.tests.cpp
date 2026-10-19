/**
 * @file FogEffect.tests.cpp
 * @author Rahul Nair
 * @brief Tests for renderer/FogEffect.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/FogEffect.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

TEST_CASE("FogEffect: ReadFogParams on an empty params object returns defaults", "[renderer]")
{
    const nlohmann::json params = nlohmann::json::object();
    const mir::FogParams fog = mir::ReadFogParams(params);

    CHECK(fog.color == mir::FogParams{}.color);
    CHECK(fog.density == mir::FogParams{}.density);
    CHECK(fog.start == mir::FogParams{}.start);
    CHECK(fog.end == mir::FogParams{}.end);
}

TEST_CASE("FogEffect: ReadFogParams falls back per field when some are missing", "[renderer]")
{
    const nlohmann::json params = { { "density", 0.5f } };
    const mir::FogParams fog = mir::ReadFogParams(params);

    CHECK(fog.density == 0.5f);
    CHECK(fog.color == mir::FogParams{}.color);
    CHECK(fog.start == mir::FogParams{}.start);
    CHECK(fog.end == mir::FogParams{}.end);
}

TEST_CASE("FogEffect: ReadFogParams ignores a malformed color array", "[renderer]")
{
    const nlohmann::json params = { { "color", "not an array" } };
    const mir::FogParams fog = mir::ReadFogParams(params);

    CHECK(fog.color == mir::FogParams{}.color);
}

TEST_CASE("FogEffect: ReadFogParams clamps end to be at least start", "[renderer]")
{
    const nlohmann::json params = { { "start", 100.0f },{ "end", 10.0f } };
    const mir::FogParams fog = mir::ReadFogParams(params);

    CHECK(fog.start == 100.0f);
    CHECK(fog.end == 100.0f);
}

TEST_CASE("FogEffect: WriteFogParams then ReadFogParams round-trips every field", "[renderer]")
{
    mir::FogParams saved;
    saved.color = { 0.1f, 0.2f, 0.3f };
    saved.density = 0.042f;
    saved.start = 5.0f;
    saved.end = 150.0f;

    nlohmann::json params = nlohmann::json::object();
    mir::WriteFogParams(params, saved);

    const mir::FogParams loaded = mir::ReadFogParams(params);
    CHECK(loaded.color == saved.color);
    CHECK(loaded.density == saved.density);
    CHECK(loaded.start == saved.start);
    CHECK(loaded.end == saved.end);
}
