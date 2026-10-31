/**
 * @file HdrEffect.h
 * @author Rahul Nair
 * @brief HDR tonemapping's shape - the single source of truth for its default
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
    inline constexpr std::string_view kHdrEffectName = "HDR";

    enum class TonemapOperator
    {
        None = 0,
        Reinhard = 1,
        ACES = 2,
    };

    // Exposure is the viewing Camera's.
    struct HdrParams
    {
        TonemapOperator op = TonemapOperator::Reinhard;

        bool operator==(const HdrParams &) const = default;
    };

    HdrParams ReadHdrParams(const nlohmann::json &params);
    void WriteHdrParams(nlohmann::json &params, const HdrParams &hdr);

    std::string_view ToString(TonemapOperator op);
    TonemapOperator ParseOperator(std::string_view name);

    void RegisterHdrPostEffect();
}
