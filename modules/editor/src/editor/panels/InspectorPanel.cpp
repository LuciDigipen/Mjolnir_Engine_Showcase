/**
 * @file InspectorPanel.cpp
 * @author Rahul Nair
 * @brief Shows and edits the Selection's components - the editor's
 *        "Inspector" dock panel.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <editor/panels/InspectorPanel.h>

#include <core/ecs/ComponentFields.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/components/Name.h>
#include <core/math/WorldReal.h>
#include <editor/EditorWidgets.h>
#include <editor/EntityBaseline.h>
#include <editor/Selection.h>
#include <editor/history/AuthoredEdit.h>
#include <editor/history/ComponentCommands.h>
#include <editor/history/History.h>
#include <editortheme/EditorTheme.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <IconsFontAwesome6.h>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        using FieldBuffer = InspectorPanel::FieldBuffer;
        using Clipboard = InspectorPanel::Clipboard;

        
        struct Edits
        {
            History &mHistory;
            IAuthoredEntities *mAuthored = nullptr;
            bool mSubmitted = false;
            bool mSnapRequested = false;
            const SnapToTerrainAction *mSnap = nullptr;

            void Submit(std::unique_ptr<ICommand> command)
            {
                mHistory.Submit(std::move(command));
                mSubmitted = true;
            }
        };

        FieldValue ToValue(uint32_t field, const FieldBuffer &buffer)
        {
            FieldValue value;
            value.mField = field;
            std::memcpy(value.mBytes, buffer.mBytes, sizeof(value.mBytes));
            return value;
        }

        constexpr const char *kValueId = "##value";

        bool DrawFieldWidget(const FieldDesc &field, void *bytes)
        {
            const char *label = kValueId;

            switch (field.mKind)
            {
            case FieldKind::Bool:
                return ImGui::Checkbox(label, static_cast<bool *>(bytes));
            case FieldKind::Int:
                return ImGui::DragInt(label, static_cast<int32_t *>(bytes));
            case FieldKind::Float:
                return ImGui::DragFloat(label, static_cast<float *>(bytes), 0.1f);
            case FieldKind::Vec3:
                return widgets::AxisDragN(label, ImGuiDataType_Float, glm::value_ptr(*static_cast<glm::vec3 *>(bytes)),
                    3, 0.1f, "%.3f");
            case FieldKind::WorldPosition:
                {
// NoRoundToFormat: a drag must not snap a far position to the 4 shown decimals
                    WorldVec3 &value = *static_cast<WorldVec3 *>(bytes);
                    constexpr ImGuiDataType kWorldDataType = kLargeWorld ? ImGuiDataType_Double : ImGuiDataType_Float;
                    if (!widgets::AxisDragN(label, kWorldDataType, glm::value_ptr(value), 3, 0.1f, "%.4f",
                        ImGuiSliderFlags_NoRoundToFormat))
                        return false;
                    // typed text such as 1e400 parses to inf, which Transform's setter drops silently
                    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
                }
            case FieldKind::Vec4:
                return widgets::AxisDragN(label, ImGuiDataType_Float, glm::value_ptr(*static_cast<glm::vec4 *>(bytes)),
                    4, 0.1f, "%.3f");
            case FieldKind::Quat:
                {
// Editing quaternion components directly can denormalize them
                    glm::quat &quat = *static_cast<glm::quat *>(bytes);
                    glm::vec3 euler = glm::degrees(glm::eulerAngles(quat));
                    if (widgets::AxisDragN(label, ImGuiDataType_Float, glm::value_ptr(euler), 3, 0.5f, "%.3f"))
                    {
                        quat = glm::quat(glm::radians(euler));
                        return true;
                    }
                    return false;
                }
            case FieldKind::Mat4:
                {
                    bool changed = false;
                    float *m = glm::value_ptr(*static_cast<glm::mat4 *>(bytes));
                    for (int row = 0; row < 4; ++row)
                    {
                        ImGui::PushID(row);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        changed |= ImGui::DragFloat4("##row", m + row * 4, 0.1f);
                        ImGui::PopID();
                    }
                    return changed;
                }
            case FieldKind::EntityRef:
                {
                    const Entity *ref = static_cast<const Entity *>(bytes);
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("Entity %u", ref->mIndex);
                    return false;
                }
            case FieldKind::Handle:
                return ImGui::DragScalarN(label, ImGuiDataType_U32, bytes, 2);
            case FieldKind::String:
                return ImGui::InputText(label, static_cast<char *>(bytes), kFieldStringCapacity);
            }
            return false;
        }

        bool DrawEnumWidget(const FieldDesc &field, void *bytes)
        {
            int32_t &value = *static_cast<int32_t *>(bytes);
            const char *preview = (value >= 0 && static_cast<size_t>(value) < field.mEnumNames.size())
                ? field.mEnumNames[static_cast<size_t>(value)].data()
                : "?";

            bool changed = false;
            if (ImGui::BeginCombo(kValueId, preview))
            {
                for (size_t i = 0; i < field.mEnumNames.size(); ++i)
                {
                    const bool isSelected = static_cast<int32_t>(i) == value;
                    if (ImGui::Selectable(field.mEnumNames[i].data(), isSelected) && !isSelected)
                    {
                        value = static_cast<int32_t>(i);
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            return changed;
        }

        bool DrawColorWidget(const FieldDesc &field, void *bytes)
        {
            float *color = reinterpret_cast<float *>(bytes);
            return field.mKind == FieldKind::Vec4 ? ImGui::ColorEdit4(kValueId, color)
                : ImGui::ColorEdit3(kValueId, color);
        }

        // Only AssetKind::Model exists today; add a branch per new AssetKind here.
        bool DrawAssetRefWidget(const FieldDesc &field, void *bytes, const AssetImportView &assets)
        {
            if (field.mAssetKind != AssetKind::Model)
                return false;

            const char *currentId = static_cast<const char *>(bytes);
            const char *preview = "Choose model...";
            for (const auto &model : assets.mModels)
            {
                char id[17]{};
                std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(model.mAssetId));
                if (std::string_view(id) == currentId)
                    preview = model.mPath.c_str();
            }

            bool changed = false;
            if (ImGui::BeginCombo(kValueId, preview))
            {
                for (const auto &model : assets.mModels)
                    if (model.mAssetId != 0 && ImGui::Selectable(model.mPath.c_str()))
                    {
                        char newId[kFieldStringCapacity]{};
                        std::snprintf(newId, sizeof(newId), "%016llx",
                            static_cast<unsigned long long>(model.mAssetId));
                        if (std::string_view(newId) != currentId)
                        {
                            std::memcpy(bytes, newId, kFieldStringCapacity);
                            changed = true;
                        }
                    }
                ImGui::EndCombo();
            }
            return changed;
        }

        
        void AddResetValues(const ComponentOps &ops, std::span<const std::string_view> names,
            std::vector<FieldValue> &values)
        {
            for (std::string_view name : names)
                if (const FieldDesc *field = ops.FindField(name))
                    values.push_back(ToValue(static_cast<uint32_t>(field - ops.mFields.data()), FieldBuffer{}));
        }

        Clipboard CopyValues(World &world, const ComponentOps &ops, Entity entity)
        {
            Clipboard clipboard{ &ops,{} };
            if (const void *component = ops.GetComponent(world, entity))
            {
                for (const FieldDesc &field : ops.mFields)
                {
                    FieldBuffer value{};
                    field.Read(component, value.mBytes);
                    clipboard.mValues.push_back(value);
                }
            }
            return clipboard;
        }

        
        void DrawAuthoredActions(Edits &edits, const AuthoredKey &key, bool canRevert)
        {
            IAuthoredEntities &authored = *edits.mAuthored;
            if (ImGui::MenuItem("Revert", nullptr, false, canRevert))
                edits.Submit(MakeRevertAuthoredCommand(authored, key));

            const std::vector<AuthoredApplyTarget> targets = authored.ApplyTargets(key);
            if (targets.empty() || !ImGui::BeginMenu("Apply to"))
                return;

            for (std::size_t index = 0; index < targets.size(); ++index)
            {
                const AuthoredAction apply = authored.Apply(key, index);
                std::string label = targets[index].mLabel;
                if (targets[index].mOverrides)
                    label += "  " ICON_FA_PEN;

                ImGui::BeginDisabled(!apply.mAllowed);
                if (ImGui::MenuItem(label.c_str()))
                    edits.Submit(MakeApplyAuthoredCommand(authored, key, index));
                ImGui::EndDisabled();
                if (!apply.mAllowed)
                    ImGui::SetItemTooltip("%s", apply.mReason.c_str());
            }
            ImGui::EndMenu();
        }

        void PasteValues(World &world, const Clipboard &clipboard, Entity entity, Edits &edits)
        {
            const ComponentOps &ops = *clipboard.mOps;

            std::vector<FieldValue> values;
            for (std::size_t i = 0; i < ops.mFields.size() && i < clipboard.mValues.size(); ++i)
                values.push_back(ToValue(static_cast<uint32_t>(i), clipboard.mValues[i]));

            if (ops.Has(world, entity))
                edits.Submit(MakeSetFieldsEditCommand(entity, ops, std::move(values),
                    "Paste " + std::string(ops.mType.name) + " Values", edits.mAuthored));
            else
                edits.Submit(MakeAddComponentEditCommand(entity, ops, std::move(values), edits.mAuthored));
        }

        // right-click menu on a component header
        void DrawComponentMenu(World &world, const ComponentOps &ops, Entity entity, Clipboard &clipboard,
            Edits &edits)
        {
            if (!ImGui::BeginPopupContextItem("ComponentMenu"))
                return;

            if (ImGui::MenuItem("Copy Values"))
                clipboard = CopyValues(world, ops, entity);
            if (ImGui::MenuItem("Paste Values", nullptr, false, clipboard.mOps == &ops))
                PasteValues(world, clipboard, entity, edits);

            if (edits.mAuthored != nullptr)
            {
                const std::optional<AuthoredKey> key = edits.mAuthored->ComponentKeyOf(entity, ops.mType.name);
                if (key && edits.mAuthored->ComponentState(entity, ops.mType.name) == AuthoredComponentState::Added)
                {
                    ImGui::Separator();
                    DrawAuthoredActions(edits, *key, true);
                }
            }

            if (edits.mSnap != nullptr && ops.mType.name == "Transform")
            {
                ImGui::Separator();
                ImGui::BeginDisabled(!edits.mSnap->mEnabled);
                if (ImGui::MenuItem("Snap to Terrain"))
                    edits.mSnapRequested = true;
                ImGui::EndDisabled();
                if (!edits.mSnap->mEnabled)
                    ImGui::SetItemTooltip("%s", edits.mSnap->mReason.c_str());
                else if (!edits.mSnap->mStatus.empty())
                    ImGui::SetItemTooltip("%s", edits.mSnap->mStatus.c_str());
            }

            ImGui::Separator();

            // shown disabled rather than hidden, so the baseline's protection is visible
            if (ImGui::MenuItem("Remove Component", nullptr, false, !IsBaselineComponent(ops.mType)))
                edits.Submit(MakeRemoveComponentEditCommand(entity, ops, edits.mAuthored));

            ImGui::EndPopup();
        }

        bool FieldVisible(const ComponentOps &ops, const FieldDesc &field, std::string_view filter)
        {
            return !field.mHidden &&
                (widgets::MatchesFilter(ops.mType.name, filter) || widgets::MatchesFilter(field.mName, filter));
        }

        bool ComponentVisible(const ComponentOps &ops, std::string_view filter)
        {
            if (widgets::MatchesFilter(ops.mType.name, filter))
                return true;
            for (const FieldDesc &field : ops.mFields)
                if (FieldVisible(ops, field, filter))
                    return true;
            return false;
        }

        void DrawComponent(World &world, const ComponentOps &ops, Entity entity, const AssetImportView &assets,
            std::string_view filter, Clipboard &clipboard, Edits &edits)
        {
            if (!ComponentVisible(ops, filter))
                return;

            ImGui::PushID(static_cast<int>(ops.mType.seq));

            const AuthoredComponentState componentState = edits.mAuthored != nullptr
                ? edits.mAuthored->ComponentState(entity, ops.mType.name)
                : AuthoredComponentState::Absent;
            std::string header(ops.mType.name);
            if (componentState == AuthoredComponentState::Added)
                header += "  " ICON_FA_PLUS;

            const bool open = widgets::CategoryHeader(header.c_str());
            DrawComponentMenu(world, ops, entity, clipboard, edits);

            if (open)
            {
                void *component = ops.GetComponent(world, entity);
                if (component == nullptr)
                {
                    ImGui::TextDisabled("(tag - no fields)");
                }
                else if (widgets::BeginPropertyGrid("##details"))
                {
                    for (uint32_t index = 0; index < ops.mFields.size(); ++index)
                    {
                        const FieldDesc &field = ops.mFields[index];
                        if (!FieldVisible(ops, field, filter))
                            continue;

                        ImGui::PushID(static_cast<int>(index));
                        FieldBuffer buffer{};
                        field.Read(component, buffer.mBytes);

                        std::optional<AuthoredKey> fieldKey;
                        AuthoredFieldState fieldState = AuthoredFieldState::Inherited;
                        if (edits.mAuthored != nullptr)
                        {
                            fieldKey = edits.mAuthored->KeyOf(entity, ops.mType.name, field.mName);
                            if (fieldKey)
                                fieldState = edits.mAuthored->FieldState(*fieldKey);
                        }

                        std::string fieldLabel(field.mName);
                        if (fieldState == AuthoredFieldState::Overridden)
                            fieldLabel += "  " ICON_FA_PEN;

                        const bool readOnly = field.ReadOnly();
                        ImGui::BeginDisabled(readOnly);
                        widgets::PropertyRow(fieldLabel.c_str());
                        bool edited = false;
                        switch (field.mWidget)
                        {
                        case FieldWidget::Enum:
                            edited = DrawEnumWidget(field, buffer.mBytes);
                            break;
                        case FieldWidget::Color:
                            edited = DrawColorWidget(field, buffer.mBytes);
                            break;
                        case FieldWidget::AssetRef:
                            edited = DrawAssetRefWidget(field, buffer.mBytes, assets);
                            break;
                        case FieldWidget::Default:
                            edited = field.mClamped
                                ? ImGui::DragFloat(kValueId, reinterpret_cast<float *>(buffer.mBytes),
                                    0.05f, field.mMin, field.mMax, "%.3f", ImGuiSliderFlags_AlwaysClamp)
                                : DrawFieldWidget(field, buffer.mBytes);
                            break;
                        }
                        ImGui::EndDisabled();

                        if (fieldKey && ImGui::BeginPopupContextItem("##authored-field"))
                        {
                            DrawAuthoredActions(edits, *fieldKey,
                                fieldState == AuthoredFieldState::Overridden);
                            ImGui::EndPopup();
                        }

                        if (edited && !readOnly)
                        {
                            std::vector<FieldValue> values{ ToValue(index, buffer) };
                            if (field.mWidget == FieldWidget::AssetRef)
                                AddResetValues(ops, field.mResetOnChange, values);
                            edits.Submit(MakeSetFieldsEditCommand(entity, ops, std::move(values), "",
                                edits.mAuthored));
                        }
                        ImGui::PopID();
                    }
                    widgets::EndPropertyGrid();
                }
            }

            ImGui::PopID();
        }

        void DrawAddComponent(World &world, Entity entity, const Clipboard &clipboard, Edits &edits)
        {
            const ImGuiStyle &style = ImGui::GetStyle();
            const float width = ImGui::CalcTextSize(ICON_FA_PLUS "Add").x + style.FramePadding.x * 2.0f +
                style.ItemInnerSpacing.x * 1.5f;
            const float nameEnd =
                ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX() + style.ItemSpacing.x;
            ImGui::SameLine(std::max(nameEnd, ImGui::GetWindowContentRegionMax().x - width));
            if (widgets::IconTextButton("##add-component", ICON_FA_PLUS, "Add",
                EditorTheme::Color(ThemeColor::Success), ImVec2(width, 0.0f)))
                ImGui::OpenPopup("AddComponent");
            ImGui::SetItemTooltip("Add a component to this entity");

            if (!ImGui::BeginPopup("AddComponent"))
                return;

            // the copied component, when this entity lacks it, added with the copied values
            if (clipboard.mOps != nullptr && !clipboard.mOps->Has(world, entity))
            {
                const std::string label = "Paste " + std::string(clipboard.mOps->mType.name);
                if (ImGui::Selectable(label.c_str()))
                    PasteValues(world, clipboard, entity, edits);
                ImGui::Separator();
            }

            bool any = false;
            for (const ComponentOps *ops : ComponentRegistry::Instance().All())
            {
                if (IsBaselineComponent(ops->mType) || ops->Has(world, entity))
                    continue;

                any = true;
                if (ImGui::Selectable(ops->mType.name.data()))
                    edits.Submit(MakeAddComponentEditCommand(entity, *ops, {}, edits.mAuthored));
            }
            if (!any)
                ImGui::TextDisabled("Every component is already added.");

            ImGui::EndPopup();
        }

        void DrawRemovedComponents(Entity entity, Edits &edits)
        {
            if (edits.mAuthored == nullptr)
                return;

            const std::vector<std::string> removed = edits.mAuthored->RemovedComponents(entity);
            if (removed.empty())
                return;

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextDisabled("Removed Components");
            for (const std::string &component : removed)
            {
                const std::optional<AuthoredKey> key = edits.mAuthored->ComponentKeyOf(entity, component);
                if (!key)
                    continue;

                ImGui::PushID(component.c_str());
                ImGui::Text(ICON_FA_MINUS "  %s", component.c_str());

                ImGui::SameLine();
                if (ImGui::SmallButton("Revert"))
                    edits.Submit(MakeRevertAuthoredCommand(*edits.mAuthored, *key));

                ImGui::SameLine();
                const AuthoredAction apply = edits.mAuthored->Apply(*key);
                ImGui::BeginDisabled(!apply.mAllowed);
                if (ImGui::SmallButton("Apply to Prefab"))
                    edits.Submit(MakeApplyAuthoredCommand(*edits.mAuthored, *key));
                ImGui::EndDisabled();
                if (!apply.mAllowed)
                    ImGui::SetItemTooltip("%s", apply.mReason.c_str());

                ImGui::PopID();
            }
        }
    } // namespace

    bool InspectorPanel::Draw(World &world, const AssetImportView &assets, History &history,
        IAuthoredEntities *authored, const SnapToTerrainAction &snap)
    {
        EndEditIfReleased(history);

        const Selection *selection = world.TryResource<Selection>();
        const size_t selectedCount = selection != nullptr ? selection->mSelected.size() : 0;

        if (selectedCount > 1)
        {
            ImGui::TextDisabled("%zu objects selected.", selectedCount);
            return false;
        }

        const Entity entity = selectedCount == 1 ? selection->mSelected.front() : kNullEntity;

        if (entity.IsNull() || !world.IsAlive(entity))
        {
            ImGui::TextDisabled("No entity selected.");
            return false;
        }

        //history applies these at the end of the frame, so nothing below makes a change to archety midway
        Edits edits{ history, authored };
        edits.mSnap = &snap;

        ImGui::AlignTextToFramePadding();
        if (const Name *name = world.GetComponent<Name>(entity))
            ImGui::Text(ICON_FA_CUBE "  %s", name->mValue);
        else
            ImGui::Text(ICON_FA_CUBE "  Entity %u", entity.mIndex);

        DrawAddComponent(world, entity, mClipboard, edits);

        widgets::SearchBox("##details-search", mFilter, sizeof(mFilter), "Search Details");
        ImGui::Spacing();

        for (const ComponentOps *ops : ComponentRegistry::Instance().All())
        {
            if (!ops->Has(world, entity))
                continue;

            DrawComponent(world, *ops, entity, assets, mFilter, mClipboard, edits);
        }

        DrawRemovedComponents(entity, edits);

        mEditing |= edits.mSubmitted;
        EndEditIfReleased(history);
        return edits.mSnapRequested;
    }

    void InspectorPanel::EndEditIfReleased(History &history)
    {
        if (!mEditing || ImGui::IsAnyItemActive())
            return;

        history.EndStep();
        mEditing = false;
    }
} // namespace mir
