/**
 * @file EditorWidgets.cpp
 * @author Rahul Nair
 * @brief UE5-style ImGui widgets shared by the editor panels.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <editor/EditorWidgets.h>

#include <editortheme/EditorTheme.h>

#include <IconsFontAwesome6.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>

namespace mir::widgets
{
    namespace
    {
        constexpr ThemeColor kAxisColors[] = { ThemeColor::AxisX, ThemeColor::AxisY, ThemeColor::AxisZ, ThemeColor::AxisW };
    }

    bool MatchesFilter(std::string_view text, std::string_view filter)
    {
        if (filter.empty())
            return true;
        const auto it = std::search(text.begin(), text.end(), filter.begin(), filter.end(),
            [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
        return it != text.end();
    }

    bool SearchBox(const char *id, char *buffer, std::size_t size, const char *hint, float width)
    {
        const ImGuiStyle &style = ImGui::GetStyle();
        const float iconWidth = ImGui::CalcTextSize(ICON_FA_MAGNIFYING_GLASS).x;

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
            ImVec2(style.FramePadding.x * 2.0f + iconWidth, style.FramePadding.y));
        ImGui::SetNextItemWidth(width);
        const bool changed = ImGui::InputTextWithHint(id, hint, buffer, size);
        ImGui::PopStyleVar();

        const ImVec2 min = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + style.FramePadding.x, min.y + style.FramePadding.y),
            ImGui::GetColorU32(ImGuiCol_TextDisabled), ICON_FA_MAGNIFYING_GLASS);
        return changed;
    }

    bool CategoryHeader(const char *label, ImGuiTreeNodeFlags flags)
    {
        const ImVec2 padding = ImGui::GetStyle().FramePadding;

        ImGui::PushStyleColor(ImGuiCol_Header, EditorTheme::Color(ThemeColor::Category));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorTheme::Color(ThemeColor::CategoryHovered));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, EditorTheme::Color(ThemeColor::CategoryHovered));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padding.x, padding.y + 1.0f));
        const bool open = ImGui::CollapsingHeader(label, flags);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        return open;
    }

    bool BeginPropertyGrid(const char *id)
    {
        // window-seeded, not ID-stack, so same-named grids share columns
        const ImGuiID tableId = ImGui::GetIDWithSeed(id, nullptr, ImGui::GetCurrentWindow()->ID);
        if (!ImGui::BeginTableEx(id, tableId, 2,
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings))
            return false;

        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.4f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
        return true;
    }

    void EndPropertyGrid()
    {
        ImGui::EndTable();
    }

    void PropertyRow(const char *label)
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::Indent(ImGui::GetStyle().IndentSpacing * 0.5f);
        ImGui::TextUnformatted(label);
        ImGui::Unindent(ImGui::GetStyle().IndentSpacing * 0.5f);

        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
    }

    bool AxisDragN(const char *id, ImGuiDataType type, void *data, int count, float speed, const char *format,
        ImGuiSliderFlags flags)
    {
        const ImGuiStyle &style = ImGui::GetStyle();
        const std::size_t typeSize = ImGui::DataTypeGetInfo(type)->Size;
        const float spacing = style.ItemInnerSpacing.x;
        const float width = std::max(1.0f, (ImGui::CalcItemWidth() - spacing * static_cast<float>(count - 1)) /
            static_cast<float>(count));
        const float tagWidth = std::max(2.0f, ImGui::GetFontSize() * 0.2f);

        bool changed = false;
        ImGui::PushID(id);
        ImGui::BeginGroup();
        for (int i = 0; i < count; ++i)
        {
            ImGui::PushID(i);
            if (i > 0)
                ImGui::SameLine(0.0f, spacing);

            ImGui::SetNextItemWidth(width);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                ImVec2(style.FramePadding.x + tagWidth, style.FramePadding.y));
            changed |= ImGui::DragScalar("##axis", type, static_cast<char *>(data) + typeSize * i, speed,
                nullptr, nullptr, format, flags);
            ImGui::PopStyleVar();

            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            const ThemeColor axis = kAxisColors[std::min(i, 3)];
            ImGui::GetWindowDrawList()->AddRectFilled(min, ImVec2(min.x + tagWidth, max.y),
                ImGui::ColorConvertFloat4ToU32(EditorTheme::Color(axis)), style.FrameRounding,
                ImDrawFlags_RoundCornersLeft);
            ImGui::PopID();
        }
        ImGui::EndGroup();
        ImGui::PopID();
        return changed;
    }

    bool ToolbarButton(const char *icon, const char *tooltip, bool active)
    {
        const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, active ? EditorTheme::Color(ThemeColor::Primary) : clear);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            active ? EditorTheme::Color(ThemeColor::Primary, 0.85f) : ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));

        const float size = ImGui::GetFrameHeight();
        const bool clicked = ImGui::Button(icon, ImVec2(size, size));

        if (active)
            ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        if (tooltip != nullptr)
            ImGui::SetItemTooltip("%s", tooltip);
        return clicked;
    }

    bool IconTextButton(const char *id, const char *icon, const char *text, const ImVec4 &iconColor, const ImVec2 &size)
    {
        const ImGuiStyle &style = ImGui::GetStyle();
        const ImVec2 iconSize = ImGui::CalcTextSize(icon);
        const ImVec2 textSize = ImGui::CalcTextSize(text, nullptr, true);
        const float gap = textSize.x > 0.0f ? style.ItemInnerSpacing.x * 1.5f : 0.0f;
        const float contentWidth = iconSize.x + gap + textSize.x;

        ImVec2 buttonSize = size;
        if (buttonSize.x <= 0.0f)
            buttonSize.x = contentWidth + style.FramePadding.x * 2.0f;
        if (buttonSize.y <= 0.0f)
            buttonSize.y = ImGui::GetFrameHeight();

        const bool clicked = ImGui::Button(id, buttonSize);

        const ImVec2 min = ImGui::GetItemRectMin();
        const float x = min.x + (buttonSize.x - contentWidth) * 0.5f;
        const float y = min.y + (buttonSize.y - ImGui::GetFontSize()) * 0.5f;
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        drawList->AddText(ImVec2(x, y), ImGui::ColorConvertFloat4ToU32(iconColor), icon);
        drawList->AddText(ImVec2(x + iconSize.x + gap, y), ImGui::GetColorU32(ImGuiCol_Text), text,
            ImGui::FindRenderedTextEnd(text));
        return clicked;
    }

    int PushRowSelectionColors(bool selected)
    {
        if (!selected)
            return 0;
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorTheme::Color(ThemeColor::SelectionHovered));
        return 1;
    }
}
