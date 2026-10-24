/**
 * @file EditorWidgets.h
 * @author Rahul Nair
 * @brief UE5-style ImGui widgets shared by the editor panels.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <imgui.h>

#include <cfloat>
#include <cstddef>
#include <string_view>

namespace mir::widgets
{
    bool MatchesFilter(std::string_view text, std::string_view filter);

    bool SearchBox(const char *id, char *buffer, std::size_t size, const char *hint = "Search",
        float width = -FLT_MIN);

    bool CategoryHeader(const char *label, ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen);

    // grids with the same id in one window share columns
    bool BeginPropertyGrid(const char *id);
    void EndPropertyGrid();
    void PropertyRow(const char *label);

    // data holds count consecutive values of type
    bool AxisDragN(const char *id, ImGuiDataType type, void *data, int count, float speed,
        const char *format = nullptr, ImGuiSliderFlags flags = 0);

    bool ToolbarButton(const char *icon, const char *tooltip, bool active = false);

    // id is a hidden label such as "##add"
    bool IconTextButton(const char *id, const char *icon, const char *text, const ImVec4 &iconColor,
        const ImVec2 &size = ImVec2(0.0f, 0.0f));

    // returns the count to pass to ImGui::PopStyleColor
    int PushRowSelectionColors(bool selected);
}
