/**
 * @file FilmEffect.h
 * @author Rahul Nair
 * @brief Film look's shape - the single source of truth for its default
 *        params and json layout, shared by VulkanRenderer (apply) and the
 *        Post-Processing panel (editing), so they can't drift apart.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <nlohmann/json.hpp>

#include <string_view>

namespace mir
{
    inline constexpr std::string_view kFilmEffectName = "Film";

    struct FilmParams
    {
        float grain = 0.0f;
        float contrast = 1.0f;
        float saturation = 1.0f;
        float vignetteStrength = 0.0f;
        float vignetteRadius = 1.0f;

        bool operator==(const FilmParams &) const = default;
    };

    FilmParams ReadFilmParams(const nlohmann::json &params);
    void WriteFilmParams(nlohmann::json &params, const FilmParams &film);

    void RegisterFilmPostEffect();
}
