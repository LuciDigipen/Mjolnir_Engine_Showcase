/**
 * @file AntiAliasingEffect.h
 * @author Rahul Nair
 * @brief Anti-aliasing's shape - the single source of truth for its default
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
    inline constexpr std::string_view kAntiAliasingEffectName = "Anti-Aliasing";

    // Off is always valid; Fxaa is raster-only; Msaa/Dlss are ray-tracing-only,
    // Dlss further gated on NGX/RTX support
    enum class AntiAliasingMode
    {
        Off = 0,
        Fxaa = 1,
        Msaa = 2,
        Dlss = 3,
    };

    struct AntiAliasingParams
    {
        AntiAliasingMode mode = AntiAliasingMode::Off;

        // Ray-traced MSAA fallback's per-pixel ray sample count; unused by Fxaa/Dlss.
        int rtSampleCount = 4;

        bool operator==(const AntiAliasingParams &) const = default;
    };

    std::string_view ToString(AntiAliasingMode mode);
    // Falls back to Off on anything unrecognized - the AA entry's safe default.
    AntiAliasingMode ParseMode(std::string_view name);

    AntiAliasingParams ReadAntiAliasingParams(const nlohmann::json &params);
    void WriteAntiAliasingParams(nlohmann::json &params, const AntiAliasingParams &aa);

    // The panel's combo (see AntiAliasingEffect.cpp) only ever lets a user select a mode valid
    // right now, but a hand-edited or cross-machine project_settings.json isn't bound by that -
    // this is the actual enforcement point for "a device without NGX/RTX support never fails"
    // and "Off renders the scene unchanged, in both pipelines". Falls back to Off.
    AntiAliasingMode ClampAntiAliasingMode(AntiAliasingMode requested, bool rayTracing, bool ngxSupported);

    void RegisterAntiAliasingPostEffect();
}
