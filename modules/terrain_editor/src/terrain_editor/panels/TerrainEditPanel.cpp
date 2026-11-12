/**
 * @file TerrainEditPanel.cpp
 * @author Jackey Cen
 * @brief Renders the editor's "Terrain" panel: edit layers, brush settings and Bake.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_editor/panels/TerrainEditPanel.h>

#include <editor/EditorWidgets.h>
#include <terrain_editor/TerrainEditMode.h>

#include <imgui.h>

#include <array>
#include <cfloat>

namespace mir
{
    namespace
    {
        struct BrushLabel
        {
            TerrainBrush mBrush;
            const char *mLabel;
        };

        constexpr std::array<BrushLabel, 4> kBrushLabels{
            BrushLabel{ TerrainBrush::RaiseLower, "Raise / Lower" },
            BrushLabel{ TerrainBrush::Smooth, "Smooth" },
            BrushLabel{ TerrainBrush::Flatten, "Flatten" },
            BrushLabel{ TerrainBrush::Paint, "Paint" },
        };

        const char *LabelOf(TerrainBrush brush)
        {
            for (const BrushLabel &label : kBrushLabels)
            {
                if (label.mBrush == brush)
                    return label.mLabel;
            }
            return kBrushLabels.front().mLabel;
        }
    }

    void TerrainEditPanel::Draw(TerrainEditMode &mode)
    {
        if (widgets::CategoryHeader("Edit Layers"))
        {
            ImGui::TextDisabled("No edit layers.");
            ImGui::BeginDisabled();
            ImGui::Button("Add Layer");
            ImGui::SameLine();
            ImGui::Button("Remove Layer");
            ImGui::EndDisabled();
        }

        if (widgets::CategoryHeader("Brush"))
        {
            if (ImGui::BeginCombo("Brush", LabelOf(mode.Brush())))
            {
                for (const BrushLabel &label : kBrushLabels)
                {
                    if (ImGui::Selectable(label.mLabel, label.mBrush == mode.Brush()))
                        mode.SetBrush(label.mBrush);
                }
                ImGui::EndCombo();
            }

            float radius = mode.Radius();
            if (ImGui::DragFloat("Radius", &radius, 0.1f, TerrainEditMode::kMinRadius, TerrainEditMode::kMaxRadius,
                "%.1f m"))
            {
                mode.SetRadius(radius);
            }

            float strength = mode.Strength();
            if (ImGui::SliderFloat("Strength", &strength, 0.0f, 1.0f))
                mode.SetStrength(strength);

            float falloff = mode.Falloff();
            if (ImGui::SliderFloat("Falloff", &falloff, 0.0f, 1.0f))
                mode.SetFalloff(falloff);
        }

        if (widgets::CategoryHeader("Bake"))
        {
            ImGui::BeginDisabled();
            ImGui::Button("Bake");
            ImGui::EndDisabled();
            ImGui::ProgressBar(0.0f, ImVec2(-FLT_MIN, 0.0f), "Idle");
        }
    }
}
