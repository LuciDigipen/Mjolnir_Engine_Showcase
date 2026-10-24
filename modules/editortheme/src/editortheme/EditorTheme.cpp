/**
 * @file EditorTheme.cpp
 * @author Rahul Nair
 * @brief Applies the selected editor theme to the live ImGuiStyle.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "editortheme/EditorTheme.h"

#include "ThemePalettes.h"

#include <imgui.h>

namespace mir
{
    namespace
    {
        using detail::FromHex;

        ImGuiStyle gBaseStyle;
        detail::ThemeColors gColors{};
    }

    void EditorTheme::Apply(EditorThemeKind kind, bool srgbTarget)
    {
        detail::gSrgbTarget = srgbTarget;

        ImGuiStyle &style = ImGui::GetStyle();
        style = ImGuiStyle();

        switch (kind)
        {
        case EditorThemeKind::Slate:
            gColors = detail::ApplySlate(style);
            break;
        case EditorThemeKind::Midnight:
            gColors = detail::ApplyMidnight(style);
            break;
        }

        gBaseStyle = style;
    }

    void EditorTheme::ScaleForDpi(float scale)
    {
        ImGuiStyle &style = ImGui::GetStyle();
        style = gBaseStyle;
        style.ScaleAllSizes(scale);
        style.FontScaleDpi = scale;
    }

    ImVec4 EditorTheme::Color(StatusColor status, float alpha)
    {
        switch (status)
        {
        case StatusColor::Muted:
            return FromHex(gColors.muted, alpha);
        case StatusColor::Info:
            return FromHex(gColors.info, alpha);
        case StatusColor::Warning:
            return FromHex(gColors.warning, alpha);
        case StatusColor::Danger:
            return FromHex(gColors.danger, alpha);
        }
        return FromHex(gColors.text, alpha);
    }

    ImVec4 EditorTheme::Color(ThemeColor color, float alpha)
    {
        switch (color)
        {
        case ThemeColor::AxisX:
            return FromHex(gColors.axisX, alpha);
        case ThemeColor::AxisY:
            return FromHex(gColors.axisY, alpha);
        case ThemeColor::AxisZ:
            return FromHex(gColors.axisZ, alpha);
        case ThemeColor::AxisW:
            return FromHex(gColors.axisW, alpha);
        case ThemeColor::Category:
            return FromHex(gColors.category, alpha);
        case ThemeColor::CategoryHovered:
            return FromHex(gColors.categoryHovered, alpha);
        case ThemeColor::SelectionHovered:
            return FromHex(gColors.selectionHovered, alpha);
        case ThemeColor::Success:
            return FromHex(gColors.success, alpha);
        case ThemeColor::Primary:
            return FromHex(gColors.primary, alpha);
        }
        return FromHex(gColors.text, alpha);
    }
}
