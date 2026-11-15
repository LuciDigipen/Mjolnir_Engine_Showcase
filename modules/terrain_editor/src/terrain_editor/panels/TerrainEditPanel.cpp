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
#include <terrain_editor/TerrainMaterialLayersDraft.h>

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

    TerrainPanelActions TerrainEditPanel::Draw(TerrainEditMode &mode, TerrainLayerWorkspace &layers, History &history,
        const TerrainBrushAvailability &brush, const TerrainBakeView &bake,
        const TerrainMaterialLayersView &materialLayersView, std::span<const std::string> materialLayerNames,
        std::string &chosenMaterial)
    {
        TerrainPanelActions actions;
        if (widgets::CategoryHeader("Edit Layers"))
            DrawLayers(layers, history);

        DrawMaterialLayers(materialLayersView, actions);

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

            if (mode.Brush() == TerrainBrush::Paint)
            {
                const char *preview = chosenMaterial.empty() ? "Choose a material" : chosenMaterial.c_str();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("Material##paint", preview))
                {
                    for (const std::string &layer : materialLayerNames)
                    {
                        if (ImGui::Selectable(layer.c_str(), layer == chosenMaterial))
                            chosenMaterial = layer;
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("Left-drag paints this material into the selected Edit Layer.\n"
                                      "Hold Ctrl to erase it.");
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
                actions.mStartBake = true;
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(!bake.mCanCancel);
            if (ImGui::Button("Cancel"))
                actions.mCancelBake = true;
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

    void TerrainEditPanel::DrawMaterialLayers(const TerrainMaterialLayersView &view, TerrainPanelActions &actions)
    {
        const bool open = widgets::CategoryHeader("Material layers");
        ImGui::SetItemTooltip("The terrain's surface layers and the height, slope and noise rules that blend them.\n"
                              "Apply writes the recipe and layered material, migrates painted weights and bakes.");
        if (open && !mMaterialSectionOpen)
            actions.mRescanMaterials = true;
        mMaterialSectionOpen = open;
        if (!open)
            return;

        if (view.mDraft != mMaterialDraftSeen ||
            (view.mDraft != nullptr && view.mDraft->RecipePath().generic_string() != mMaterialRecipeSeen))
        {
            mMaterialDraftSeen = view.mDraft;
            if (view.mDraft != nullptr)
                mMaterialRecipeSeen = view.mDraft->RecipePath().generic_string();
            mMaterialRenameRow = -1;
            mMaterialRenameError.clear();
            mMaterialDeleteRow = -1;
        }

        if (!view.mLoadError.empty())
        {
            ImGui::PushTextWrapPos(0.0f);
            TextError(view.mLoadError);
            ImGui::PopTextWrapPos();
            return;
        }
        if (view.mDraft == nullptr)
        {
            ImGui::TextDisabled("No terrain asset is selected.");
            return;
        }

        TerrainMaterialLayersDraft &draft = *view.mDraft;
        ImGui::PushID("material-layers");
        if (ImGui::BeginTable("##table", 3,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Layer");
            ImGui::TableSetupColumn("Surface Material");
            ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();

            for (std::size_t index = 0; index < draft.Layers().size(); ++index)
            {
                TerrainMaterialLayerRow &row = draft.Row(index);
                ImGui::PushID(static_cast<int>(index));
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                if (mMaterialRenameRow == static_cast<int>(index))
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    const bool submitted = ImGui::InputText("##name", mMaterialRenameBuffer,
                        sizeof(mMaterialRenameBuffer),
                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                    {
                        mMaterialRenameRow = -1;
                        mMaterialRenameError.clear();
                    }
                    else if (submitted)
                    {
                        const std::string name = mMaterialRenameBuffer;
                        const bool used = std::ranges::any_of(draft.Layers(),
                            [&](const TerrainMaterialLayerRow &other) { return &other != &row && other.name == name; });
                        if (!IsValidTerrainMaterialName(name))
                            mMaterialRenameError =
                                "Layer names use lowercase letters, digits or underscores, and cannot be empty.";
                        else if (used)
                            mMaterialRenameError = "Another layer already uses that name.";
                        else
                        {
                            row.name = name;
                            mMaterialRenameRow = -1;
                            mMaterialRenameError.clear();
                        }
                    }
                }
                else if (ImGui::Selectable(row.name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    std::snprintf(mMaterialRenameBuffer, sizeof(mMaterialRenameBuffer), "%s", row.name.c_str());
                    mMaterialRenameRow = static_cast<int>(index);
                    mMaterialRenameError.clear();
                }
                ImGui::SetItemTooltip("Double-click to rename; painted weights follow the layer.");

                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-FLT_MIN);
                const char *preview = row.material.empty() ? "Choose a material" : row.material.c_str();
                if (ImGui::BeginCombo("##material", preview))
                {
                    for (const std::string &material : view.mMaterials)
                    {
                        if (ImGui::Selectable(material.c_str(), material == row.material))
                            row.material = material;
                    }
                    ImGui::EndCombo();
                }
                const bool missing =
                    std::find(view.mMaterials.begin(), view.mMaterials.end(), row.material) == view.mMaterials.end();
                if (!row.material.empty() && missing)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(not found)");
                }

                ImGui::TableSetColumnIndex(2);
                ImGui::BeginDisabled(index == 0);
                if (ImGui::Button("Up##row"))
                    draft.MoveLayer(index, -1);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(index + 1 >= draft.Layers().size());
                if (ImGui::Button("Down##row"))
                    draft.MoveLayer(index, 1);
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Delete##row"))
                {
                    // Painted channels still carry the name the row loaded with.
                    const std::string &channel = row.originalName.empty() ? row.name : row.originalName;
                    const bool painted = std::find(view.mPaintedLayers.begin(), view.mPaintedLayers.end(), channel) !=
                        view.mPaintedLayers.end();
                    if (painted)
                        mMaterialDeleteRow = static_cast<int>(index);
                    else
                        draft.RemoveLayer(index);
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        if (mMaterialDeleteRow >= 0)
        {
            if (mMaterialDeleteRow >= static_cast<int>(draft.Layers().size()))
                mMaterialDeleteRow = -1;
            else
            {
                const std::string name = draft.Layers()[static_cast<std::size_t>(mMaterialDeleteRow)].name;
                ImGui::PushTextWrapPos(0.0f);
                ImGui::Text("Delete layer '%s'? Its painted weights are removed too.", name.c_str());
                ImGui::PopTextWrapPos();
                if (ImGui::Button("Delete##confirm"))
                {
                    draft.RemoveLayer(static_cast<std::size_t>(mMaterialDeleteRow));
                    mMaterialDeleteRow = -1;
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel##confirm"))
                    mMaterialDeleteRow = -1;
            }
        }

        ImGui::BeginDisabled(!draft.CanAddLayer());
        if (ImGui::Button("Add Layer##material"))
            draft.AddLayer();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Adds a layer that blends over the ones before it.");

        ImGui::SameLine();
        if (ImGui::Button("Refresh Materials"))
            actions.mRescanMaterials = true;
        ImGui::SetItemTooltip("Rescans the assets root for surface materials that declare the terrain frontend.");

        const auto dragMeters = [](const char *label, double &value, double low, double high) {
            float edited = static_cast<float>(value);
            if (ImGui::DragFloat(label, &edited, 0.1f, static_cast<float>(low), static_cast<float>(high), "%.2f m"))
                value = static_cast<double>(edited);
        };
        const auto dragDegrees = [](const char *label, double &value, double low, double high) {
            float edited = static_cast<float>(value);
            if (ImGui::DragFloat(label, &edited, 0.1f, static_cast<float>(low), static_cast<float>(high), "%.1f deg"))
                value = static_cast<double>(edited);
        };

        for (std::size_t index = 1; index < draft.Layers().size(); ++index)
        {
            TerrainMaterialRules &rules = draft.MutableRules(index);
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::TreeNodeEx("##rules", ImGuiTreeNodeFlags_None, "Rules: %s", draft.Layers()[index].name.c_str()))
            {
                bool height = rules.height.has_value();
                if (ImGui::Checkbox("Height", &height))
                {
                    rules.height = height ? std::optional<TerrainMaterialHeightRule>(TerrainMaterialHeightRule{})
                                          : std::nullopt;
                }
                if (rules.height)
                {
                    TerrainMaterialHeightRule &rule = *rules.height;
                    ImGui::SetNextItemWidth(120.0f);
                    dragMeters("Min##height", rule.min, -kMaxTerrainHeightMagnitude, kMaxTerrainHeightMagnitude);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(120.0f);
                    dragMeters("Max##height", rule.max, -kMaxTerrainHeightMagnitude, kMaxTerrainHeightMagnitude);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(120.0f);
                    dragMeters("Blend##height", rule.blend, 0.0, kMaxTerrainHeightMagnitude);
                }

                bool slope = rules.slope.has_value();
                if (ImGui::Checkbox("Slope", &slope))
                    rules.slope = slope ? std::optional<TerrainMaterialSlopeRule>(TerrainMaterialSlopeRule{})
                                        : std::nullopt;
                if (rules.slope)
                {
                    TerrainMaterialSlopeRule &rule = *rules.slope;
                    ImGui::SetNextItemWidth(120.0f);
                    dragDegrees("Min##slope", rule.min, 0.0, kMaxTerrainMaterialSlopeDegrees);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(120.0f);
                    dragDegrees("Max##slope", rule.max, 0.0, kMaxTerrainMaterialSlopeDegrees);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(120.0f);
                    dragDegrees("Blend##slope", rule.blend, 0.0, kMaxTerrainMaterialSlopeDegrees);
                }

                bool noise = rules.noise.has_value();
                if (ImGui::Checkbox("Noise", &noise))
                    rules.noise = noise ? std::optional<TerrainMaterialNoiseRule>(TerrainMaterialNoiseRule{})
                                        : std::nullopt;
                if (rules.noise)
                {
                    TerrainMaterialNoiseRule &rule = *rules.noise;
                    ImGui::SetNextItemWidth(120.0f);
                    dragMeters("Wavelength##noise", rule.wavelength, kMinTerrainNoiseWavelength,
                        kMaxTerrainNoiseWavelength);
                    ImGui::SameLine();
                    int octaves = rule.octaves;
                    ImGui::SetNextItemWidth(120.0f);
                    if (ImGui::SliderInt("Octaves##noise", &octaves, 1, kMaxTerrainMaterialNoiseOctaves))
                        rule.octaves = octaves;
                    ImGui::SameLine();
                    float strength = static_cast<float>(rule.strength);
                    ImGui::SetNextItemWidth(120.0f);
                    if (ImGui::SliderFloat("Strength##noise", &strength, 0.0f, 1.0f))
                        rule.strength = static_cast<double>(strength);
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        TextError(mMaterialRenameError);

        ImGui::Separator();
        ImGui::BeginDisabled(!view.mCanApply);
        if (ImGui::Button("Apply##material"))
            actions.mApplyMaterials = true;
        ImGui::SetItemTooltip("Writes the recipe and layered material, migrates painted weights and bakes.");
        ImGui::SameLine();
        if (ImGui::Button("Revert##material"))
            actions.mRevertMaterials = true;
        ImGui::SetItemTooltip("Reloads the draft from disk.");
        ImGui::EndDisabled();

        if (!view.mDisabledReason.empty())
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", view.mDisabledReason.c_str());
            ImGui::PopTextWrapPos();
        }
        if (view.mFailed)
            TextError(view.mStatus);
        else if (!view.mStatus.empty())
            ImGui::TextUnformatted(view.mStatus.c_str());

        ImGui::PopID();
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
