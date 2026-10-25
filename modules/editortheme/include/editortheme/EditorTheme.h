/**
 * @file EditorTheme.h
 * @author Rahul Nair
 * @brief The editor themes: Midnight (default) and Slate.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

struct ImVec4;

namespace mir
{
    enum class StatusColor
    {
        Muted,
        Info,
        Warning,
        Danger,
    };

    enum class ThemeColor
    {
        AxisX,
        AxisY,
        AxisZ,
        AxisW,
        Category,
        CategoryHovered,
        SelectionHovered,
        Success,
        Primary,
        Prefab,
    };

    enum class EditorThemeKind
    {
        Midnight,
        Slate,
    };

    class EditorTheme
    {
    public:
        EditorTheme() = delete;

        // resets the live ImGuiStyle; call ScaleForDpi after
        static void Apply(EditorThemeKind kind = EditorThemeKind::Midnight, bool srgbTarget = true);
        static void ScaleForDpi(float scale);
        static ImVec4 Color(StatusColor status, float alpha = 1.0f);
        static ImVec4 Color(ThemeColor color, float alpha = 1.0f);
    };
}
