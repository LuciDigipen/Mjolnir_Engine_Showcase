/**
 * @file BloomEffect.tests.cpp
 * @author Rahul Nair
 * @brief Tests for renderer/BloomEffect.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/BloomEffect.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

TEST_CASE("BloomEffect: ReadBloomParams on an empty params object returns defaults", "[renderer]")
{
    const nlohmann::json params = nlohmann::json::object();
    const mir::BloomParams bloom = mir::ReadBloomParams(params);

    CHECK(bloom.threshold == mir::BloomParams{}.threshold);
    CHECK(bloom.intensity == mir::BloomParams{}.intensity);
    CHECK(bloom.radius == mir::BloomParams{}.radius);
}

TEST_CASE("BloomEffect: ReadBloomParams falls back per field when some are missing", "[renderer]")
{
    const nlohmann::json params = { { "intensity", 1.25f } };
    const mir::BloomParams bloom = mir::ReadBloomParams(params);

    CHECK(bloom.threshold == mir::BloomParams{}.threshold);
    CHECK(bloom.intensity == 1.25f);
    CHECK(bloom.radius == mir::BloomParams{}.radius);
}

TEST_CASE("BloomEffect: WriteBloomParams then ReadBloomParams round-trips every field", "[renderer]")
{
    mir::BloomParams saved;
    saved.threshold = 1.75f;
    saved.intensity = 0.9f;
    saved.radius = 2.5f;

    nlohmann::json params = nlohmann::json::object();
    mir::WriteBloomParams(params, saved);

    const mir::BloomParams loaded = mir::ReadBloomParams(params);
    CHECK(loaded.threshold == saved.threshold);
    CHECK(loaded.intensity == saved.intensity);
    CHECK(loaded.radius == saved.radius);
}
