/**
 * @file ThemePalettes.h
 * @author Rahul Nair
 * @brief The palettes behind EditorTheme, one source file per theme.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cmath>
#include <cstdint>

#include <imgui.h>

namespace mir::detail
{
    inline float SrgbToLinear(uint32_t channel)
    {
        const float c = static_cast<float>(channel & 0xFF) / 255.0f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }

    inline bool gSrgbTarget = true;

    // an sRGB swapchain re-encodes output, so hand it linear values for the hex to show as written
    inline ImVec4 FromHex(uint32_t hex, float alpha = 1.0f)
    {
        if (!gSrgbTarget)
            return ImVec4(static_cast<float>((hex >> 16) & 0xFF) / 255.0f,
                static_cast<float>((hex >> 8) & 0xFF) / 255.0f, static_cast<float>(hex & 0xFF) / 255.0f, alpha);
        return ImVec4(SrgbToLinear(hex >> 16), SrgbToLinear(hex >> 8), SrgbToLinear(hex), alpha);
    }

    struct ThemeColors
    {
        uint32_t text;
        uint32_t muted;
        uint32_t info;
        uint32_t warning;
        uint32_t danger;
        uint32_t success;
        uint32_t primary;
        uint32_t axisX;
        uint32_t axisY;
        uint32_t axisZ;
        uint32_t axisW;
        uint32_t category;
        uint32_t categoryHovered;
        uint32_t selectionHovered;
        uint32_t prefab;
    };

    ThemeColors ApplySlate(ImGuiStyle &style);
    ThemeColors ApplyMidnight(ImGuiStyle &style);
}
