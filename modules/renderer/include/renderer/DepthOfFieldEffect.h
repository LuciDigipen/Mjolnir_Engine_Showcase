/**
 * @file DepthOfFieldEffect.h
 * @author Rahul Nair
 * @brief Depth of field's shape - the single source of truth for its default
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
    inline constexpr std::string_view kDepthOfFieldEffectName = "DepthOfField";

    struct DepthOfFieldParams
    {
        float focusDistance = 10.0f;
        float aperture = 0.1f;
        float maxBlur = 16.0f;

        bool operator==(const DepthOfFieldParams &) const = default;
    };

    DepthOfFieldParams ReadDepthOfFieldParams(const nlohmann::json &params);
    void WriteDepthOfFieldParams(nlohmann::json &params, const DepthOfFieldParams &dof);

    void RegisterDepthOfFieldPostEffect();
}
