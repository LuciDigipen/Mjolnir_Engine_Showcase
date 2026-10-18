/**
 * @file FogEffect.h
 * @author Rahul Nair
 * @brief Distance fog's shape - the single source of truth for its default
 *        params and json layout, shared by RenderSystem (fallback/apply)
 *        and the Post-Processing panel (editing), so they can't drift apart.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <nlohmann/json.hpp>

#include <glm/vec3.hpp>

#include <string_view>

namespace mir
{
    inline constexpr std::string_view kFogEffectName = "Fog";

    struct FogParams
    {
        glm::vec3 color{ 0.5f, 0.6f, 0.7f };
        float density = 0.015f;
        float start = 0.0f;
        float end = 300.0f;
    };

    FogParams ReadFogParams(const nlohmann::json &params);
    void WriteFogParams(nlohmann::json &params, const FogParams &fog);

    void RegisterFogPostEffect();
}
