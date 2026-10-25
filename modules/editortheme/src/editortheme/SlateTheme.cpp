/**
 * @file SlateTheme.cpp
 * @author Rahul Nair
 * @brief The Slate editor theme: blue-grey panels, rounded frames.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ThemePalettes.h"

namespace mir::detail
{
    namespace
    {
        constexpr uint32_t kText = 0xE6E8EC;
        constexpr uint32_t kTextDisabled = 0x6B7280;
        constexpr uint32_t kWindow = 0x1B1E24;
        constexpr uint32_t kChild = 0x16181D;
        constexpr uint32_t kPopup = 0x1E2128;
        constexpr uint32_t kBorder = 0x2A2E37;
        constexpr uint32_t kFrame = 0x272B33;
        constexpr uint32_t kFrameHovered = 0x323742;
        constexpr uint32_t kFrameActive = 0x3A404C;
        constexpr uint32_t kTitleBg = 0x16181D;
        constexpr uint32_t kTitleBgActive = 0x1E2128;
        constexpr uint32_t kMenuBarBg = 0x1B1E24;
        constexpr uint32_t kScrollbarBg = 0x16181D;
        constexpr uint32_t kScrollbarGrab = 0x353A45;
        constexpr uint32_t kScrollbarGrabHovered = 0x424855;
        constexpr uint32_t kScrollbarGrabActive = 0x525966;
        constexpr uint32_t kButton = 0x2B303A;
        constexpr uint32_t kButtonHovered = 0x39404C;
        constexpr uint32_t kHeader = 0x2B303A;
        constexpr uint32_t kHeaderHovered = 0x39404C;
        constexpr uint32_t kTab = 0x16181D;
        constexpr uint32_t kTabHovered = 0x2B303A;
        constexpr uint32_t kTabSelected = 0x272B33;
        constexpr uint32_t kTabDimmed = 0x131519;
        constexpr uint32_t kTabDimmedSelected = 0x1E2128;
        constexpr uint32_t kAccent = 0x4C8DFF;
        constexpr uint32_t kAccentDim = 0x3665B3;

        constexpr uint32_t kStatusInfo = 0x6FA8DC;
        constexpr uint32_t kStatusWarning = 0xE0A63C;
        constexpr uint32_t kStatusDanger = 0xE5555A;
        constexpr uint32_t kStatusSuccess = 0x4CAF50;

        constexpr uint32_t kAxisX = 0xD9534F;
        constexpr uint32_t kAxisY = 0x6CB33E;
        constexpr uint32_t kAxisZ = 0x4C8DFF;

        constexpr uint32_t kPrefab = 0x8AB8FF;
    }

    ThemeColors ApplySlate(ImGuiStyle &style)
    {
        style.FramePadding = ImVec2(8.0f, 5.0f);
        style.ItemSpacing = ImVec2(8.0f, 6.0f);
        style.ItemInnerSpacing = ImVec2(6.0f, 5.0f);
        style.WindowPadding = ImVec2(10.0f, 10.0f);

        style.WindowRounding = 0.0f;
        style.ChildRounding = 4.0f;
        style.PopupRounding = 4.0f;
        style.FrameRounding = 4.0f;
        style.TabRounding = 4.0f;
        style.GrabRounding = 4.0f;
        style.ScrollbarRounding = 9.0f;

        style.WindowMenuButtonPosition = ImGuiDir_None;
        style.WindowTitleAlign = ImVec2(0.0f, 0.5f);

        ImVec4 *colors = style.Colors;

        colors[ImGuiCol_Text] = FromHex(kText);
        colors[ImGuiCol_TextDisabled] = FromHex(kTextDisabled);

        colors[ImGuiCol_WindowBg] = FromHex(kWindow);
        colors[ImGuiCol_ChildBg] = FromHex(kChild);
        colors[ImGuiCol_PopupBg] = FromHex(kPopup);
        colors[ImGuiCol_Border] = FromHex(kBorder);

        colors[ImGuiCol_FrameBg] = FromHex(kFrame);
        colors[ImGuiCol_FrameBgHovered] = FromHex(kFrameHovered);
        colors[ImGuiCol_FrameBgActive] = FromHex(kFrameActive);

        colors[ImGuiCol_TitleBg] = FromHex(kTitleBg);
        colors[ImGuiCol_TitleBgActive] = FromHex(kTitleBgActive);
        colors[ImGuiCol_MenuBarBg] = FromHex(kMenuBarBg);

        colors[ImGuiCol_ScrollbarBg] = FromHex(kScrollbarBg);
        colors[ImGuiCol_ScrollbarGrab] = FromHex(kScrollbarGrab);
        colors[ImGuiCol_ScrollbarGrabHovered] = FromHex(kScrollbarGrabHovered);
        colors[ImGuiCol_ScrollbarGrabActive] = FromHex(kScrollbarGrabActive);

        colors[ImGuiCol_Button] = FromHex(kButton);
        colors[ImGuiCol_ButtonHovered] = FromHex(kButtonHovered);
        colors[ImGuiCol_ButtonActive] = FromHex(kAccent);

        colors[ImGuiCol_Header] = FromHex(kHeader);
        colors[ImGuiCol_HeaderHovered] = FromHex(kHeaderHovered);
        colors[ImGuiCol_HeaderActive] = FromHex(kAccent, 0.85f);

        colors[ImGuiCol_Tab] = FromHex(kTab);
        colors[ImGuiCol_TabHovered] = FromHex(kTabHovered);
        colors[ImGuiCol_TabSelected] = FromHex(kTabSelected);
        colors[ImGuiCol_TabDimmed] = FromHex(kTabDimmed);
        colors[ImGuiCol_TabDimmedSelected] = FromHex(kTabDimmedSelected);

        colors[ImGuiCol_DockingPreview] = FromHex(kAccent);

        colors[ImGuiCol_CheckMark] = FromHex(kAccent);
        colors[ImGuiCol_SliderGrab] = FromHex(kAccentDim);
        colors[ImGuiCol_SliderGrabActive] = FromHex(kAccent);
        colors[ImGuiCol_DragDropTarget] = FromHex(kAccent);

        return {
            .text = kText,
            .muted = kTextDisabled,
            .info = kStatusInfo,
            .warning = kStatusWarning,
            .danger = kStatusDanger,
            .success = kStatusSuccess,
            .primary = kAccent,
            .axisX = kAxisX,
            .axisY = kAxisY,
            .axisZ = kAxisZ,
            .axisW = kTextDisabled,
            .category = kHeader,
            .categoryHovered = kHeaderHovered,
            .selectionHovered = kHeaderHovered,
            .prefab = kPrefab,
        };
    }
}
