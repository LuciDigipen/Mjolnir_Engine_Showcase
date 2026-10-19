/**
 * @file FilmEffect.tests.cpp
 * @author Rahul Nair
 * @brief Tests for renderer/FilmEffect.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/FilmEffect.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

TEST_CASE("FilmEffect: ReadFilmParams on an empty params object returns defaults", "[renderer]")
{
    const nlohmann::json params = nlohmann::json::object();
    const mir::FilmParams film = mir::ReadFilmParams(params);

    CHECK(film.grain == mir::FilmParams{}.grain);
    CHECK(film.contrast == mir::FilmParams{}.contrast);
    CHECK(film.saturation == mir::FilmParams{}.saturation);
    CHECK(film.vignetteStrength == mir::FilmParams{}.vignetteStrength);
    CHECK(film.vignetteRadius == mir::FilmParams{}.vignetteRadius);
}

TEST_CASE("FilmEffect: ReadFilmParams falls back per field when some are missing", "[renderer]")
{
    const nlohmann::json params = { { "grain", 0.2f },{ "vignetteStrength", 0.5f } };
    const mir::FilmParams film = mir::ReadFilmParams(params);

    CHECK(film.grain == 0.2f);
    CHECK(film.contrast == mir::FilmParams{}.contrast);
    CHECK(film.saturation == mir::FilmParams{}.saturation);
    CHECK(film.vignetteStrength == 0.5f);
    CHECK(film.vignetteRadius == mir::FilmParams{}.vignetteRadius);
}

TEST_CASE("FilmEffect: WriteFilmParams then ReadFilmParams round-trips every field", "[renderer]")
{
    mir::FilmParams saved;
    saved.grain = 0.15f;
    saved.contrast = 1.2f;
    saved.saturation = 0.8f;
    saved.vignetteStrength = 0.6f;
    saved.vignetteRadius = 1.5f;

    nlohmann::json params = nlohmann::json::object();
    mir::WriteFilmParams(params, saved);

    const mir::FilmParams loaded = mir::ReadFilmParams(params);
    CHECK(loaded.grain == saved.grain);
    CHECK(loaded.contrast == saved.contrast);
    CHECK(loaded.saturation == saved.saturation);
    CHECK(loaded.vignetteStrength == saved.vignetteStrength);
    CHECK(loaded.vignetteRadius == saved.vignetteRadius);
}
