/**
 * @file MotionBlurEffect.h
 * @author Rahul Nair
 * @brief Motion blur's shape - the single source of truth for its default
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
    inline constexpr std::string_view kMotionBlurEffectName = "MotionBlur";

    struct MotionBlurParams
    {
        float shutterStrength = 0.5f;
        int sampleCount = 8;

        bool operator==(const MotionBlurParams &) const = default;
    };

    MotionBlurParams ReadMotionBlurParams(const nlohmann::json &params);
    void WriteMotionBlurParams(nlohmann::json &params, const MotionBlurParams &motionBlur);

    void RegisterMotionBlurPostEffect();
}
