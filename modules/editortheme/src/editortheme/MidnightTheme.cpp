/**
 * @file MidnightTheme.cpp
 * @author Rahul Nair
 * @brief The Midnight editor theme, modelled on Unreal Engine 5's dark editor.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ThemePalettes.h"

namespace mir::detail
{
    namespace
    {
        constexpr uint32_t kForeground = 0xC0C0C0;
        constexpr uint32_t kForegroundDisabled = 0x6E6E6E;
        constexpr uint32_t kTextSelected = 0x0050A0;
        constexpr uint32_t kCaret = 0xE0E0E0;

        constexpr uint32_t kTitle = 0x0A0A0A;
        constexpr uint32_t kRecessed = 0x101010;
        constexpr uint32_t kPanel = 0x161616;
        constexpr uint32_t kPopup = 0x1A1A1A;
        constexpr uint32_t kBorder = 0x2A2A2A;

        constexpr uint32_t kInput = 0x0A0A0A;
        constexpr uint32_t kInputHovered = 0x101010;
        constexpr uint32_t kInputActive = 0x141414;

        constexpr uint32_t kSecondary = 0x262626;
        constexpr uint32_t kSecondaryHovered = 0x323232;

        constexpr uint32_t kCategory = 0x222222;
        constexpr uint32_t kCategoryHovered = 0x2A2A2A;

        constexpr uint32_t kRowHovered = 0x222222;
        constexpr uint32_t kSelect = 0x0050A0;
        constexpr uint32_t kSelectHovered = 0x0060C0;

        constexpr uint32_t kTabHovered = 0x1C1C1C;

        constexpr uint32_t kScrollbarGrab = 0x2C2C2C;
        constexpr uint32_t kScrollbarGrabHovered = 0x3A3A3A;
        constexpr uint32_t kScrollbarGrabActive = 0x505050;

        constexpr uint32_t kTableHeader = 0x1E1E1E;
        constexpr uint32_t kTableBorderStrong = 0x2A2A2A;
        constexpr uint32_t kTableBorderLight = 0x1E1E1E;
        constexpr uint32_t kTreeLines = 0x2A2A2A;

        constexpr uint32_t kPrimary = 0x0070E0;
        constexpr uint32_t kPrimaryHovered = 0x0E86FF;
        constexpr uint32_t kPrimaryPressed = 0x0050A0;

        constexpr uint32_t kStatusInfo = 0x3D9BFF;
        constexpr uint32_t kStatusWarning = 0xFFB800;
        constexpr uint32_t kStatusDanger = 0xEF3535;
        constexpr uint32_t kStatusSuccess = 0x26BB4F;

        constexpr uint32_t kAxisX = 0xD63B3B;
        constexpr uint32_t kAxisY = 0x5BB035;
        constexpr uint32_t kAxisZ = 0x2F80ED;
        constexpr uint32_t kAxisW = 0x808080;
    }

    ThemeColors ApplyMidnight(ImGuiStyle &style)
    {
        style.WindowPadding = ImVec2(10.0f, 10.0f);
        style.FramePadding = ImVec2(8.0f, 6.0f);
        style.ItemSpacing = ImVec2(8.0f, 6.0f);
        style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
        style.CellPadding = ImVec2(6.0f, 4.0f);
        style.IndentSpacing = 16.0f;
        style.ScrollbarSize = 10.0f;
        style.ScrollbarPadding = 2.0f;
        style.GrabMinSize = 10.0f;
        style.SeparatorTextBorderSize = 1.0f;
        style.SeparatorTextPadding = ImVec2(16.0f, 6.0f);

        style.WindowBorderSize = 1.0f;
        style.ChildBorderSize = 1.0f;
        style.PopupBorderSize = 1.0f;
        style.FrameBorderSize = 1.0f;
        style.TabBorderSize = 0.0f;
        style.TabBarBorderSize = 1.0f;
        style.TabBarOverlineSize = 1.0f;
        style.DockingSeparatorSize = 2.0f;

        style.WindowRounding = 4.0f;
        style.ChildRounding = 4.0f;
        style.PopupRounding = 4.0f;
        style.FrameRounding = 4.0f;
        style.GrabRounding = 4.0f;
        style.TabRounding = 4.0f;
        style.ScrollbarRounding = 12.0f;
        style.MenuItemRounding = 4.0f;

        style.TreeLinesFlags = ImGuiTreeNodeFlags_DrawLinesToNodes;
        style.TreeLinesSize = 1.0f;
        style.TreeLinesRounding = 4.0f;

        style.WindowMenuButtonPosition = ImGuiDir_None;
        style.WindowTitleAlign = ImVec2(0.0f, 0.5f);

        ImVec4 *colors = style.Colors;

        colors[ImGuiCol_Text] = FromHex(kForeground);
        colors[ImGuiCol_TextDisabled] = FromHex(kForegroundDisabled);
        colors[ImGuiCol_TextSelectedBg] = FromHex(kTextSelected);
        colors[ImGuiCol_TextLink] = FromHex(kStatusInfo);
        colors[ImGuiCol_InputTextCursor] = FromHex(kCaret);

        colors[ImGuiCol_WindowBg] = FromHex(kPanel);
        colors[ImGuiCol_ChildBg] = FromHex(kRecessed);
        colors[ImGuiCol_PopupBg] = FromHex(kPopup);
        colors[ImGuiCol_Border] = FromHex(kBorder);
        colors[ImGuiCol_BorderShadow] = FromHex(0x000000, 0.0f);

        colors[ImGuiCol_FrameBg] = FromHex(kInput);
        colors[ImGuiCol_FrameBgHovered] = FromHex(kInputHovered);
        colors[ImGuiCol_FrameBgActive] = FromHex(kInputActive);

        colors[ImGuiCol_TitleBg] = FromHex(kTitle);
        colors[ImGuiCol_TitleBgActive] = FromHex(kTitle);
        colors[ImGuiCol_TitleBgCollapsed] = FromHex(kTitle);
        colors[ImGuiCol_MenuBarBg] = FromHex(kTitle);

        colors[ImGuiCol_ScrollbarBg] = FromHex(0x000000, 0.0f);
        colors[ImGuiCol_ScrollbarGrab] = FromHex(kScrollbarGrab);
        colors[ImGuiCol_ScrollbarGrabHovered] = FromHex(kScrollbarGrabHovered);
        colors[ImGuiCol_ScrollbarGrabActive] = FromHex(kScrollbarGrabActive);

        colors[ImGuiCol_Button] = FromHex(kSecondary);
        colors[ImGuiCol_ButtonHovered] = FromHex(kSecondaryHovered);
        colors[ImGuiCol_ButtonActive] = FromHex(kPrimaryPressed);

        // Header is row selection; category bars use ThemeColor::Category
        colors[ImGuiCol_Header] = FromHex(kSelect);
        colors[ImGuiCol_HeaderHovered] = FromHex(kRowHovered);
        colors[ImGuiCol_HeaderActive] = FromHex(kPrimary);

        colors[ImGuiCol_Separator] = FromHex(kBorder);
        colors[ImGuiCol_SeparatorHovered] = FromHex(kPrimary);
        colors[ImGuiCol_SeparatorActive] = FromHex(kPrimary);

        colors[ImGuiCol_ResizeGrip] = FromHex(0x000000, 0.0f);
        colors[ImGuiCol_ResizeGripHovered] = FromHex(kPrimary, 0.6f);
        colors[ImGuiCol_ResizeGripActive] = FromHex(kPrimary);

        colors[ImGuiCol_Tab] = FromHex(kTitle);
        colors[ImGuiCol_TabHovered] = FromHex(kTabHovered);
        colors[ImGuiCol_TabSelected] = FromHex(kPanel);
        colors[ImGuiCol_TabSelectedOverline] = FromHex(kPrimary);
        colors[ImGuiCol_TabDimmed] = FromHex(kTitle);
        colors[ImGuiCol_TabDimmedSelected] = FromHex(kPanel);
        colors[ImGuiCol_TabDimmedSelectedOverline] = FromHex(0x000000, 0.0f);

        colors[ImGuiCol_DockingPreview] = FromHex(kPrimary, 0.5f);
        colors[ImGuiCol_DockingEmptyBg] = FromHex(kRecessed);

        colors[ImGuiCol_TableHeaderBg] = FromHex(kTableHeader);
        colors[ImGuiCol_TableBorderStrong] = FromHex(kTableBorderStrong);
        colors[ImGuiCol_TableBorderLight] = FromHex(kTableBorderLight);
        colors[ImGuiCol_TableRowBg] = FromHex(0x000000, 0.0f);
        colors[ImGuiCol_TableRowBgAlt] = FromHex(0xFFFFFF, 0.02f);
        colors[ImGuiCol_TreeLines] = FromHex(kTreeLines);

        colors[ImGuiCol_CheckMark] = FromHex(kCaret);
        colors[ImGuiCol_SliderGrab] = FromHex(kPrimary);
        colors[ImGuiCol_SliderGrabActive] = FromHex(kPrimaryHovered);
        colors[ImGuiCol_PlotLines] = FromHex(kPrimaryHovered);
        colors[ImGuiCol_PlotLinesHovered] = FromHex(kStatusWarning);
        colors[ImGuiCol_PlotHistogram] = FromHex(kPrimary);
        colors[ImGuiCol_PlotHistogramHovered] = FromHex(kPrimaryHovered);

        colors[ImGuiCol_DragDropTarget] = FromHex(kPrimary);
        colors[ImGuiCol_DragDropTargetBg] = FromHex(kPrimary, 0.1f);
        colors[ImGuiCol_NavCursor] = FromHex(kPrimary);
        colors[ImGuiCol_NavWindowingHighlight] = FromHex(kPrimary, 0.7f);
        colors[ImGuiCol_NavWindowingDimBg] = FromHex(0x000000, 0.4f);
        colors[ImGuiCol_ModalWindowDimBg] = FromHex(0x000000, 0.6f);

        return {
            .text = kForeground,
            .muted = kForegroundDisabled,
            .info = kStatusInfo,
            .warning = kStatusWarning,
            .danger = kStatusDanger,
            .success = kStatusSuccess,
            .primary = kPrimary,
            .axisX = kAxisX,
            .axisY = kAxisY,
            .axisZ = kAxisZ,
            .axisW = kAxisW,
            .category = kCategory,
            .categoryHovered = kCategoryHovered,
            .selectionHovered = kSelectHovered,
        };
    }
}
