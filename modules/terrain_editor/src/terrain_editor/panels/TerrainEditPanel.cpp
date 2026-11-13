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
#include <editor/history/History.h>
#include <terrain_editor/TerrainEditBase.h>
#include <terrain_editor/TerrainEditMode.h>
#include <terrain_editor/TerrainLayerCommands.h>
#include <terrain_editor/TerrainLayerDocument.h>
#include <terrain_editor/TerrainLayerWorkspace.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

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

        const char *BlendLabel(TerrainEditBlend blend)
        {
            return blend == TerrainEditBlend::AlphaBlend ? "Alpha Blend" : "Additive";
        }

        const ImVec4 kErrorColor(1.0f, 0.4f, 0.4f, 1.0f);

        void TextError(const std::string &text)
        {
            if (!text.empty())
                ImGui::TextColored(kErrorColor, "%s", text.c_str());
        }

        std::size_t OrderPosition(const std::vector<std::size_t> &order, std::optional<uint32_t> sequence,
            const TerrainLayerDocument &document)
        {
            if (!sequence)
                return order.size();
            const auto found = std::find(order.begin(), order.end(), document.IndexOf(*sequence));
            return found == order.end() ? order.size() : static_cast<std::size_t>(found - order.begin());
        }
    } // namespace

    TerrainBakeActions TerrainEditPanel::Draw(TerrainEditMode &mode, TerrainLayerWorkspace &layers, History &history,
        const TerrainBrushAvailability &brush, const TerrainBakeView &bake)
    {
        TerrainBakeActions actions;
        if (widgets::CategoryHeader("Edit Layers"))
            DrawLayers(layers, history);

        const bool brushOpen = widgets::CategoryHeader("Brush");
        ImGui::SetItemTooltip("Left-drag in the viewport to sculpt the selected edit layer (hold Ctrl to lower).\n"
                              "Edits show at once in the view, collision and Play; each stroke is one undo step.");
        if (brushOpen)
        {
            if (ImGui::BeginCombo("Brush##type", LabelOf(mode.Brush())))
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

            if (!brush.message.empty())
            {
                ImGui::PushTextWrapPos(0.0f);
                if (brush.enabled)
                    ImGui::TextDisabled("%s", brush.message.c_str());
                else
                    TextError(brush.message);
                ImGui::PopTextWrapPos();
            }
        }

        const bool bakeOpen = widgets::CategoryHeader("Bake");
        ImGui::SetItemTooltip("Saves the layers, then runs TerrainTool sync in the background to publish a new release,\n"
                              "the same one a build makes. Distant regions and coarse LODs then show the edits.");
        if (bakeOpen)
        {
            ImGui::BeginDisabled(!bake.mCanStart);
            if (ImGui::Button("Bake##start"))
                actions.mStart = true;
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(!bake.mCanCancel);
            if (ImGui::Button("Cancel"))
                actions.mCancel = true;
            ImGui::EndDisabled();

            if (bake.mBaking)
            {
                // TerrainTool reports no progress, so the bar only shows that work is happening.
                ImGui::ProgressBar(-static_cast<float>(ImGui::GetTime()), ImVec2(-FLT_MIN, 0.0f),
                    bake.mStatus.c_str());
            }
            else if (bake.mFailed)
                TextError(bake.mStatus);
            else if (!bake.mStatus.empty())
                ImGui::TextUnformatted(bake.mStatus.c_str());

            if (!bake.mReason.empty())
            {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", bake.mReason.c_str());
                ImGui::PopTextWrapPos();
            }
        }

        return actions;
    }

    void TerrainEditPanel::DrawLayers(TerrainLayerWorkspace &layers, History &history)
    {
        const std::span<const TerrainEditAsset> assets = layers.Assets();
        if (assets.empty())
        {
            ImGui::TextDisabled("No terrain assets in the scene.");
            return;
        }

        if (assets.size() > 1)
        {
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##terrain-asset", assets[layers.SelectedAsset()].logicalPath.c_str()))
            {
                for (std::size_t i = 0; i < assets.size(); ++i)
                {
                    if (ImGui::Selectable(assets[i].logicalPath.c_str(), i == layers.SelectedAsset()))
                    {
                        layers.SelectAsset(i);
                        mRenameSequence.reset();
                        mRenameError.clear();
                    }
                }
                ImGui::EndCombo();
            }
        }
        else
            ImGui::TextDisabled("%s", assets.front().logicalPath.c_str());

        TerrainLayerDocument *document = layers.SelectedDocument();
        if (document == nullptr)
            return;

        if (!document->Editable())
        {
            ImGui::PushTextWrapPos(0.0f);
            TextError(document->LoadError());
            ImGui::PopTextWrapPos();
            return;
        }

        std::optional<uint32_t> selected = layers.SelectedLayer();
        if (selected && document->Find(*selected) == nullptr)
            selected.reset();
        const std::vector<std::size_t> order = document->CompositingOrder();
        if (!selected && !order.empty())
            selected = document->Layers()[order.back()].info.sequence;
        layers.SelectLayer(selected);

        DrawLayerTable(*document, layers, history);

        const std::size_t position = OrderPosition(order, selected, *document);
        const bool canMoveUp = position + 1 < order.size();
        const bool canMoveDown = position < order.size() && position > 0;

        ImGui::BeginDisabled(!selected);
        if (ImGui::Button("Delete"))
            history.Submit(std::make_unique<DeleteTerrainLayerCommand>(*document, *selected), true);
        ImGui::SameLine();
        if (ImGui::Button("Rename"))
        {
            const TerrainEditLayer *layer = document->Find(*selected);
            std::snprintf(mRenameBuffer, sizeof(mRenameBuffer), "%s", layer->info.name.c_str());
            mRenameSequence = selected;
            mRenameError.clear();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(!canMoveUp);
        if (ImGui::Button("Move Up"))
            history.Submit(std::make_unique<MoveTerrainLayerCommand>(*document, *selected, TerrainLayerMove::Up), true);
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(!canMoveDown);
        if (ImGui::Button("Move Down"))
            history.Submit(std::make_unique<MoveTerrainLayerCommand>(*document, *selected, TerrainLayerMove::Down), true);
        ImGui::EndDisabled();

        int blend = static_cast<int>(mNewLayerBlend);
        ImGui::SetNextItemWidth(130.0f);
        if (ImGui::Combo("New Layer Blend", &blend, "Additive\0Alpha Blend\0"))
            mNewLayerBlend = static_cast<TerrainEditBlend>(blend);
        ImGui::SameLine();
        if (ImGui::Button("Add Layer"))
            history.Submit(std::make_unique<AddTerrainLayerCommand>(*document, mNewLayerBlend), true);

        ImGui::BeginDisabled(!document->Unsaved());
        if (ImGui::Button("Save Layers"))
            document->Save();
        ImGui::EndDisabled();
        if (document->Unsaved())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(unsaved)");
        }

        TextError(document->SaveError());
        TextError(mRenameError);
    }

    void TerrainEditPanel::DrawLayerTable(TerrainLayerDocument &document, TerrainLayerWorkspace &layers,
        History &history)
    {
        const std::vector<std::size_t> order = document.CompositingOrder();
        if (order.empty())
            ImGui::TextDisabled("No edit layers.");

        const std::optional<uint32_t> selected = layers.SelectedLayer();
        if (!ImGui::BeginTable("##edit-layers", 4,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
            return;

        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Layer");
        ImGui::TableSetupColumn("Blend", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();

        for (auto it = order.rbegin(); it != order.rend(); ++it)
        {
            const TerrainEditLayer &layer = document.Layers()[*it];
            ImGui::PushID(static_cast<int>(layer.info.sequence));
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            bool enabled = layer.info.enabled;
            if (ImGui::Checkbox("##enabled", &enabled))
            {
                history.Submit(
                    std::make_unique<SetTerrainLayerEnabledCommand>(document, layer.info.sequence, enabled), true);
            }

            ImGui::TableSetColumnIndex(1);
            if (mRenameSequence == layer.info.sequence)
            {
                ImGui::SetNextItemWidth(-FLT_MIN);
                const bool submitted = ImGui::InputText("##rename", mRenameBuffer, sizeof(mRenameBuffer),
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                    mRenameSequence.reset();
                else if (submitted)
                {
                    if (!document.CanRename(layer.info.sequence, mRenameBuffer))
                        mRenameError = "The name is empty or another layer already uses it.";
                    else
                    {
                        history.Submit(std::make_unique<RenameTerrainLayerCommand>(document, layer.info.sequence,
                            mRenameBuffer), true);
                        mRenameSequence.reset();
                        mRenameError.clear();
                    }
                }
            }
            else if (ImGui::Selectable(layer.info.name.c_str(), selected == layer.info.sequence))
            {
                layers.SelectLayer(layer.info.sequence);
                mRenameSequence.reset();
                mRenameError.clear();
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s", BlendLabel(layer.info.heightBlend));

            ImGui::TableSetColumnIndex(3);
            int32_t priority = layer.info.priority;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputInt("##priority", &priority, 0, 0))
            {
                history.Submit(
                    std::make_unique<SetTerrainLayerPriorityCommand>(document, layer.info.sequence, priority), true);
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
} // namespace mir
