/**
 * @file BloomEffect.h
 * @author Rahul Nair
 * @brief Bloom's shape - the single source of truth for its default params
 *        and json layout, shared by VulkanRenderer (apply) and the
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
    inline constexpr std::string_view kBloomEffectName = "Bloom";

    struct BloomParams
    {
        float threshold = 1.0f;
        float intensity = 0.5f;
        float radius = 1.0f;

        bool operator==(const BloomParams &) const = default;
    };

    BloomParams ReadBloomParams(const nlohmann::json &params);
    void WriteBloomParams(nlohmann::json &params, const BloomParams &bloom);

    void RegisterBloomPostEffect();
}
