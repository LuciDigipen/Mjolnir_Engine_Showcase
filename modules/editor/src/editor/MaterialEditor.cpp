/**
 * @file MaterialEditor.cpp
 * @author Jackey Cen
 * @brief The reflected Material panel, its per-asset drafts and cook state.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "MaterialEditor.h"

#include <core/fs/FileIO.h>
#include <core/fs/Paths.h>
#include <editor/EditorWidgets.h>
#include <editortheme/EditorTheme.h>
#include <renderer/SurfaceMaterialLibrary.h>
#include <renderer/SurfaceMaterialTexture.h>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace mir
{
    namespace
    {
        constexpr double kLoadRetrySeconds = 0.5;

        const AssetImportModelView *FindImport(const AssetImportView &view, std::string_view logicalPath)
        {
            const auto found = std::find_if(view.mModels.begin(), view.mModels.end(),
                [&](const AssetImportModelView &model) { return model.mPath == logicalPath; });
            return found != view.mModels.end() ? &*found : nullptr;
        }

        bool ImportPending(AssetImportStatus status)
        {
            return status == AssetImportStatus::Queued || status == AssetImportStatus::Processing ||
                status == AssetImportStatus::Ready;
        }

        bool ImportFailed(AssetImportStatus status)
        {
            return status == AssetImportStatus::Failed || status == AssetImportStatus::Missing ||
                status == AssetImportStatus::Canceled;
        }

        uint64_t FileSignature(const std::filesystem::path &path)
        {
            std::error_code error;
            const auto time = std::filesystem::last_write_time(path, error);
            if (error)
                return 0;
            const uint64_t size = std::filesystem::file_size(path, error);
            return static_cast<uint64_t>(time.time_since_epoch().count()) ^
                (size + 0x9e3779b97f4a7c15ull + (static_cast<uint64_t>(time.time_since_epoch().count()) << 6));
        }

        MaterialRecipe RecipeFromCooked(const MaterialCookedView &view, const std::filesystem::path &source)
        {
            MaterialRecipe recipe;
            recipe.path = source;
            recipe.shaderType = view.shaderType;
            recipe.parametersJson = view.parametersJson;
            recipe.textures = view.textures;
            recipe.frontends = view.frontends;
            recipe.resolution = view.resolution;
            return recipe;
        }

        MaterialTextureSource *FindTexture(std::vector<MaterialTextureSource> &textures, std::string_view role)
        {
            const auto found = std::find_if(textures.begin(), textures.end(),
                [&](const MaterialTextureSource &texture) { return texture.mRole == role; });
            return found != textures.end() ? &*found : nullptr;
        }

        const char *ImportStatusText(AssetImportStatus status)
        {
            switch (status)
            {
            case AssetImportStatus::Ready:
                return "ready";
            case AssetImportStatus::Queued:
                return "queued";
            case AssetImportStatus::Processing:
                return "cooking";
            case AssetImportStatus::WaitingForApply:
                return "waiting for apply";
            case AssetImportStatus::UpToDate:
                return "up to date";
            case AssetImportStatus::Failed:
                return "failed";
            case AssetImportStatus::WaitingForStop:
                return "waiting for Stop";
            case AssetImportStatus::Applied:
                return "applied";
            case AssetImportStatus::Missing:
                return "missing";
            case AssetImportStatus::Canceled:
                return "canceled";
            }
            return "unknown";
        }
    } // namespace

    void MaterialEditor::Select(std::optional<std::string> logicalPath)
    {
        if (logicalPath == mSelection)
            return;
        mSelection = std::move(logicalPath);
        mLastLoadAttempt = {};
    }

    bool MaterialEditor::HasUnsavedDrafts() const
    {
        return mDrafts.HasDirty() || mDrafts.HasApplyFailed();
    }

    MaterialDraft *MaterialEditor::ActiveDraft()
    {
        return mState == TargetState::Ready && !mActiveIdentity.empty() ? mDrafts.Find(mActiveIdentity) : nullptr;
    }

    void MaterialEditor::Draw(bool *visible, bool editing, const AssetImportView &import,
        const MaterialEditorContext &context)
    {
        UpdateCooks(import, context);
        RefreshDrafts(context);
        SyncTarget(import, context);

        if (visible != nullptr && *visible)
        {
            if (ImGui::Begin("Material", visible))
            {
                switch (mState)
                {
                case TargetState::None:
                    ImGui::TextDisabled("Select a .material.json in the Content Browser.");
                    break;
                case TargetState::Loading:
                    ImGui::TextDisabled("%s", mLoadError.c_str());
                    break;
                case TargetState::Invalid:
                    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(StatusColor::Danger));
                    ImGui::TextWrapped("%s", mLoadError.c_str());
                    ImGui::PopStyleColor();
                    break;
                case TargetState::Ready:
                    DrawDraft(editing, import, context);
                    break;
                }
            }
            ImGui::End();
        }

        PreviewDrafts(editing, context);
    }

    void MaterialEditor::SyncTarget(const AssetImportView &import, const MaterialEditorContext &context)
    {
        if (!mSelection)
        {
            mState = TargetState::None;
            mActiveIdentity.clear();
            mLoadError.clear();
            return;
        }

        const std::optional<std::string> identity =
            MaterialDraftIdentity(context.mProjectKey, *mSelection);
        if (!identity)
        {
            mState = TargetState::Invalid;
            mActiveIdentity.clear();
            mLoadError = "The selected material path is not a normalized asset path.";
            return;
        }

        if (*identity != mActiveIdentity)
        {
            mActiveIdentity = *identity;
            mState = TargetState::Loading;
            mLoadError.clear();
            mSourceSignature = 0;
            mLastLoadAttempt = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        }

        if (mState == TargetState::Ready)
            return;

        const auto now = std::chrono::steady_clock::now();
        if (mLastLoadAttempt != std::chrono::steady_clock::time_point{} &&
            now - mLastLoadAttempt < std::chrono::duration<double>(kLoadRetrySeconds))
            return;
        mLastLoadAttempt = now;
        LoadTarget(import, context);
    }

    bool MaterialEditor::LoadTarget(const AssetImportView &import, const MaterialEditorContext &context)
    {
        mState = TargetState::Loading;
        mLoadError.clear();
        if (!mSelection || !context.mCatalog)
        {
            mState = TargetState::Invalid;
            mLoadError = "The material catalog is not available.";
            return false;
        }

        const std::string logicalPath = *mSelection;
        const AssetImportModelView *model = FindImport(import, logicalPath);
        const bool pending = model != nullptr && ImportPending(model->mStatus);

        const std::optional<std::filesystem::path> source =
            MaterialSourceFile(context.mSourceAssetRoot, context.mAssetIdRoot, logicalPath);
        if (!source)
        {
            mState = TargetState::Invalid;
            mLoadError = "The selection is not a material under the project's assets root.";
            return false;
        }

        std::error_code existsError;
        if (!std::filesystem::is_regular_file(*source, existsError))
        {
            mState = pending ? TargetState::Loading : TargetState::Invalid;
            mLoadError = pending ? "Waiting for the selected material to be imported."
                                 : "The material source file is missing: " + source->string();
            return false;
        }

        std::string error;
        if (!context.mLoadCooked)
        {
            mState = TargetState::Invalid;
            mLoadError = "The cooked material reader is not available.";
            return false;
        }
        MaterialCookedView cooked;
        if (!context.mLoadCooked(logicalPath, cooked, error))
        {
            mState = pending ? TargetState::Loading : TargetState::Invalid;
            mLoadError = pending ? "Waiting for the selected material to be cooked." : error;
            return false;
        }
        if (cooked.frontends.empty())
        {
            mState = TargetState::Invalid;
            mLoadError = "The material declares no frontend.";
            return false;
        }

        std::string frontend = mFrontendOverride;
        if (frontend.empty() ||
            std::find(cooked.frontends.begin(), cooked.frontends.end(), frontend) == cooked.frontends.end())
            frontend = cooked.frontends.front();

        const SurfaceMaterialRegistration *registration = context.mCatalog->FindByAsset(logicalPath, frontend);
        if (registration == nullptr)
        {
            mState = pending ? TargetState::Loading : TargetState::Invalid;
            mLoadError = pending ? "Waiting for the selected material to be cooked."
                                 : "The material has no cooked registration for frontend '" + frontend + "'.";
            return false;
        }

        const std::optional<SurfaceMaterialSchema> schema = LoadSurfaceMaterialSchema(*registration, &error);
        if (!schema)
        {
            mState = pending ? TargetState::Loading : TargetState::Invalid;
            mLoadError = "The compiled material reflection is unusable: " + error;
            return false;
        }

        // A shader-only field addition or deletion needs no recook: fields the
        // cooked recipe lacks take the validated catalog or typed default, and
        // keys the new layout dropped stay authored and reported as unsupported.
        const std::optional<std::string> complete = MergeMaterialParameterDefaults(schema->layout,
            cooked.parametersJson, registration->parametersJson, &error);
        if (!complete)
        {
            mState = pending ? TargetState::Loading : TargetState::Invalid;
            mLoadError = "The material parameters do not fit the compiled reflection: " + error;
            return false;
        }
        cooked.parametersJson = *complete;

        mRecipe = RecipeFromCooked(cooked, *source);
        mLayout = schema->layout;
        mFrontends = mRecipe.frontends;
        mFrontendOverride = frontend;
        mSourceSignature = FileSignature(*source);
        const uint64_t generation = context.mMaterials != nullptr ? context.mMaterials->Generation() : 0;
        const uint64_t acceptedRevision = MaterialAcceptedRevision(cooked.shaderType, cooked.parametersJson,
            cooked.textures, cooked.frontends);

        MaterialDraft *existing = mDrafts.Find(mActiveIdentity);
        if (existing != nullptr && existing->mCooking)
        {
            mState = TargetState::Ready;
            return true;
        }
        if (existing != nullptr && existing->mApplyFailed && acceptedRevision == existing->mAcceptedRevision)
        {
            mState = TargetState::Ready;
            return true;
        }
        if (existing != nullptr)
        {
            std::string refreshError;
            const std::optional<MaterialDraftRefresh> refresh =
                RefreshMaterialDraft(*existing, mRecipe, mLayout, frontend, schema->revision, &refreshError);
            if (!refresh)
            {
                mState = TargetState::Invalid;
                mLoadError = refreshError;
                return false;
            }
            existing->mSourceSignature = mSourceSignature;
            existing->mGeneration = generation;
            existing->mAcceptedRevision = acceptedRevision;
            existing->mSchemaRevision = schema->revision;
            existing->mSchemaStale = false;
            existing->mApplyFailed = false;
            mState = TargetState::Ready;
            return true;
        }

        std::optional<MaterialDraft> draft = BuildMaterialDraft(mActiveIdentity, logicalPath, context.mProjectKey,
            frontend, mRecipe, mLayout, &error);
        if (!draft)
        {
            mState = TargetState::Invalid;
            mLoadError = error;
            return false;
        }
        draft->mSourceSignature = mSourceSignature;
        draft->mGeneration = generation;
        draft->mAcceptedRevision = acceptedRevision;
        draft->mSchemaRevision = schema->revision;
        mDrafts.Put(std::move(*draft));
        mState = TargetState::Ready;
        return true;
    }

    void MaterialEditor::RefreshDrafts(const MaterialEditorContext &context)
    {
        if (context.mCatalog == nullptr || !context.mLoadCooked)
            return;

        for (MaterialDraft *draft : mDrafts.All())
        {
            if (draft->mCooking)
                continue;
            const std::optional<std::filesystem::path> source =
                MaterialSourceFile(context.mSourceAssetRoot, context.mAssetIdRoot, draft->mLogicalPath);
            if (!source)
            {
                draft->mSchemaStale = true;
                draft->mStatusIsError = true;
                draft->mStatus = "The material is not under the project's assets root.";
                continue;
            }
            const uint64_t signature = FileSignature(*source);

            const SurfaceMaterialRegistration *registration =
                context.mCatalog->FindByAsset(draft->mLogicalPath, draft->mFrontend);
            if (registration == nullptr)
            {
                draft->mSchemaStale = true;
                draft->mStatusIsError = true;
                draft->mStatus = "The material has no cooked registration for this frontend.";
                continue;
            }
            std::string error;
            const std::optional<SurfaceMaterialSchema> schema = LoadSurfaceMaterialSchema(*registration, &error);
            if (!schema)
            {
                draft->mSchemaStale = true;
                draft->mStatusIsError = true;
                draft->mStatus = "The compiled material reflection is unusable: " + error;
                continue;
            }

            MaterialCookedView cooked;
            if (!context.mLoadCooked(draft->mLogicalPath, cooked, error))
            {
                draft->mStatusIsError = false;
                draft->mStatus = "Waiting for the material cook to accept the current source.";
                continue;
            }
            const std::optional<std::string> complete = MergeMaterialParameterDefaults(schema->layout,
                cooked.parametersJson, registration->parametersJson, &error);
            if (!complete)
            {
                draft->mSchemaStale = true;
                draft->mStatusIsError = true;
                draft->mStatus = "The material parameters do not fit the compiled reflection: " + error;
                continue;
            }
            cooked.parametersJson = *complete;

            const uint64_t acceptedRevision = MaterialAcceptedRevision(cooked.shaderType, cooked.parametersJson,
                cooked.textures, cooked.frontends);
            const MaterialAcceptedRefresh decision =
                DecideMaterialAcceptedRefresh(*draft, acceptedRevision, schema->revision);
            if (decision == MaterialAcceptedRefresh::None)
            {
                if (signature != draft->mSourceSignature)
                {
                    draft->mStatusIsError = false;
                    draft->mStatus = "Source changed; waiting for the material cook to accept it.";
                }
                continue;
            }

            const MaterialRecipe recipe = RecipeFromCooked(cooked, *source);
            std::string refreshError;
            const std::optional<MaterialDraftRefresh> refresh =
                RefreshMaterialDraft(*draft, recipe, schema->layout, draft->mFrontend, schema->revision, &refreshError);
            if (!refresh)
            {
                draft->mSchemaStale = true;
                draft->mStatusIsError = true;
                draft->mStatus = refreshError;
                continue;
            }

            const bool acceptedChanged = acceptedRevision != draft->mAcceptedRevision;
            draft->mSourceSignature = signature;
            draft->mGeneration = context.mMaterials != nullptr ? context.mMaterials->Generation() : 0;
            draft->mAcceptedRevision = acceptedRevision;
            draft->mSchemaRevision = schema->revision;
            draft->mSchemaStale = false;
            if (acceptedChanged)
                draft->mApplyFailed = false;
            if (!refresh->mMessages.empty())
            {
                std::string message = "Schema refreshed:";
                for (const std::string &line : refresh->mMessages)
                    message += " " + line + ";";
                draft->mStatusIsError = false;
                draft->mStatus = std::move(message);
            }
        }
    }

    void MaterialEditor::UpdateCooks(const AssetImportView &import, const MaterialEditorContext &context)
    {
        for (MaterialDraft *draft : mDrafts.All())
        {
            if (!draft->mCooking)
                continue;
            const AssetImportModelView *model = FindImport(import, draft->mLogicalPath);
            const bool failed = model != nullptr && ImportFailed(model->mStatus);
            const bool requiresApply = model != nullptr &&
                (model->mStatus == AssetImportStatus::WaitingForApply ||
                    model->mStatus == AssetImportStatus::Applied);
            const std::string diagnostic = model == nullptr
                ? std::string()
                : (model->mDiagnostic.empty()
                          ? "The material cook did not apply; the last good material is unchanged."
                          : "The material cook failed: " + model->mDiagnostic);
            const uint64_t generation = context.mMaterials != nullptr ? context.mMaterials->Generation() : 0;
            std::string message;
            const MaterialCookOutcome outcome = ResolveMaterialCook(*draft, model != nullptr, failed, diagnostic,
                requiresApply,
                context.mMaterialCookRevision ? context.mMaterialCookRevision(draft->mLogicalPath)
                                               : std::optional<uint64_t>{},
                context.mMaterialCookFailureRevision
                    ? context.mMaterialCookFailureRevision(draft->mLogicalPath)
                    : std::optional<uint64_t>{},
                generation, &message);
            if (outcome == MaterialCookOutcome::Applied)
            {
                MaterialDraftCooked(*draft, true, {});
            }
            else if (outcome == MaterialCookOutcome::Failed)
            {
                MaterialDraftCooked(*draft, false, message);
            }
            else
            {
                draft->mStatusIsError = false;
                draft->mStatus = message;
            }
        }
    }

    void MaterialEditor::PreviewDrafts(bool editing, const MaterialEditorContext &context)
    {
        if (!editing || context.mMaterials == nullptr)
            return;

        const uint64_t generation = context.mMaterials->Generation();
        for (MaterialDraft *draft : mDrafts.All())
        {
            if (draft->mSchemaStale || draft->mCooking)
                continue;
            const bool active = context.mMaterials->IsActiveAsset(draft->mLogicalPath);
            const std::optional<std::string> parameters =
                MaterialDraftPreviewParameters(*draft, editing, active, generation);
            if (!parameters)
                continue;

            std::string error;
            if (context.mMaterials->PreviewParameters(draft->mLogicalPath, draft->mFrontend, *parameters, &error))
                MaterialDraftPreviewed(*draft, generation, *parameters);
            else
                MaterialDraftPreviewRejected(*draft, generation, *parameters, error);
        }
    }

    void MaterialEditor::DrawDraft(bool editing, const AssetImportView &import, const MaterialEditorContext &context)
    {
        MaterialDraft *draft = ActiveDraft();
        if (draft == nullptr)
        {
            ImGui::TextDisabled("No material is loaded.");
            return;
        }

        ImGui::TextUnformatted(draft->mLogicalPath.c_str());
        ImGui::TextDisabled("Shader type: %s", draft->mShaderType.c_str());
        ImGui::TextWrapped("Shared asset: these values change every draw that uses it, not only the selected object. "
                           "Material edits are not scene or Prefab undo steps.");
        ImGui::Separator();

        if (draft->mFrontends.size() > 1)
        {
            ImGui::BeginDisabled(!editing || MaterialDraftNeedsSave(*draft) || draft->mCooking);
            if (ImGui::BeginCombo("Frontend", draft->mFrontend.c_str()))
            {
                for (const std::string &frontend : draft->mFrontends)
                {
                    const bool selected = frontend == draft->mFrontend;
                    if (ImGui::Selectable(frontend.c_str(), selected) && !selected)
                    {
                        mFrontendOverride = frontend;
                        mActiveIdentity.clear();
                        mSourceSignature = 0;
                        mLastLoadAttempt = {};
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
        }
        else
        {
            ImGui::TextDisabled("Frontend: %s", draft->mFrontend.c_str());
        }

        if (!editing)
        {
            ImGui::TextDisabled("Play is read-only for shared materials. Stop to edit this draft.");
        }
        if (draft->mSchemaStale)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(StatusColor::Danger));
            ImGui::TextWrapped("The current source or reflection could not be accepted; preview is paused.");
            ImGui::PopStyleColor();
        }
        for (const std::string &message : draft->mSchemaMessages)
            ImGui::BulletText("%s", message.c_str());

        DrawControls(editing);

        const bool dirty = MaterialDraftDirty(*draft);
        const bool needsSave = MaterialDraftNeedsSave(*draft);
        const bool active = context.mMaterials != nullptr &&
            context.mMaterials->IsActiveAsset(draft->mLogicalPath);
        const bool blockedByPreview = active && !draft->mPreviewError.empty();
        ImGui::BeginDisabled(!editing || draft->mCooking || draft->mSchemaStale || !needsSave ||
            !MaterialDraftFinite(*draft) || blockedByPreview);
        if (ImGui::Button("Save"))
            SaveDraft(editing, context);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!editing || draft->mCooking || !needsSave);
        if (ImGui::Button("Revert"))
            RevertDraft(editing);
        ImGui::EndDisabled();
        if (draft->mCooking)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Cooking...");
        }
        else if (draft->mApplyFailed)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Source has unapplied changes; Save repairs or retries.");
        }
        else if (dirty)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Draft has unsaved changes.");
        }

        if (!draft->mPreviewError.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(StatusColor::Danger));
            ImGui::TextWrapped("Preview rejected: %s", draft->mPreviewError.c_str());
            ImGui::PopStyleColor();
        }
        if (!draft->mStatus.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text,
                EditorTheme::Color(draft->mStatusIsError ? StatusColor::Danger : StatusColor::Info));
            ImGui::TextWrapped("%s", draft->mStatus.c_str());
            ImGui::PopStyleColor();
        }

        if (const AssetImportModelView *model = FindImport(import, draft->mLogicalPath))
        {
            ImGui::TextDisabled("Import: %s", ImportStatusText(model->mStatus));
            if (!model->mDiagnostic.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(StatusColor::Danger));
                ImGui::TextWrapped("%s", model->mDiagnostic.c_str());
                ImGui::PopStyleColor();
            }
        }
    }

    void MaterialEditor::DrawControls(bool editing)
    {
        MaterialDraft *draft = ActiveDraft();
        if (draft == nullptr)
            return;

        ImGui::BeginDisabled(!editing);
        if (widgets::BeginPropertyGrid("MaterialFields"))
        {
            for (std::size_t i = 0; i < draft->mFields.size(); ++i)
            {
                MaterialDraftField &field = draft->mFields[i];
                ImGui::PushID(static_cast<int>(i));
                widgets::PropertyRow(field.mLayout.name.c_str());
                switch (field.mLayout.kind)
                {
                case MaterialFieldKind::Float:
                    ImGui::DragFloat("##value", &field.mComponents[0], 0.01f, 0.0f, 0.0f, "%.6g");
                    break;
                case MaterialFieldKind::Float2:
                    widgets::AxisDragN("##value", ImGuiDataType_Float, field.mComponents.data(), 2, 0.01f, "%.6g");
                    break;
                case MaterialFieldKind::Float3:
                    if (MaterialFieldIsColor(field.mLayout.name))
                        ImGui::ColorEdit3("##value", field.mComponents.data());
                    else
                        widgets::AxisDragN("##value", ImGuiDataType_Float, field.mComponents.data(), 3, 0.01f,
                            "%.6g");
                    break;
                case MaterialFieldKind::Float4:
                    if (MaterialFieldIsColor(field.mLayout.name))
                        ImGui::ColorEdit4("##value", field.mComponents.data());
                    else
                        widgets::AxisDragN("##value", ImGuiDataType_Float, field.mComponents.data(), 4, 0.01f,
                            "%.6g");
                    break;
                case MaterialFieldKind::Uint:
                    ImGui::InputScalar("##value", ImGuiDataType_U32, &field.mUnsigned);
                    break;
                case MaterialFieldKind::Int:
                    ImGui::InputInt("##value", &field.mSigned);
                    break;
                case MaterialFieldKind::Bool:
                    ImGui::Checkbox("##value", &field.mBool);
                    break;
                case MaterialFieldKind::TextureRef:
                    break;
                }
                ImGui::PopID();
            }
            widgets::EndPropertyGrid();
        }

        for (MaterialTextureControl &control : draft->mTextureControls)
        {
            if (!control.mEditable)
            {
                ImGui::TextDisabled("%s: %s", control.mField.c_str(), control.mReason.c_str());
                continue;
            }
            if (!widgets::CategoryHeader(control.mField.c_str()))
                continue;
            for (const std::string &role : control.mRecipeRoles)
            {
                MaterialTextureSource *source = FindTexture(draft->mTextures, role);
                if (source == nullptr)
                    continue;
                ImGui::PushID(role.c_str());
                ImGui::TextUnformatted(role.c_str());
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-120.0f);
                char buffer[512];
                std::snprintf(buffer, sizeof(buffer), "%s", source->mPath.c_str());
                if (ImGui::InputText("##path", buffer, sizeof(buffer)))
                    source->mPath = buffer;
                ImGui::SameLine();
                ImGui::TextDisabled("%s", source->mColorSpace.c_str());
                ImGui::PopID();
            }
        }

        if (!draft->mUnsupported.empty())
        {
            if (ImGui::TreeNode("Unsupported fields (preserved)"))
            {
                for (const std::string &message : draft->mUnsupported)
                    ImGui::BulletText("%s", message.c_str());
                ImGui::TreePop();
            }
        }
        ImGui::EndDisabled();
    }

    void MaterialEditor::SaveDraft(bool editing, const MaterialEditorContext &context)
    {
        MaterialDraft *draft = ActiveDraft();
        if (!editing || draft == nullptr)
            return;
        if (!MaterialDraftNeedsSave(*draft))
        {
            draft->mStatusIsError = false;
            draft->mStatus = "No changes to save.";
            return;
        }
        if (!MaterialDraftFinite(*draft))
        {
            draft->mStatusIsError = true;
            draft->mStatus = "A material value is not finite; the draft was not saved.";
            return;
        }
        if (draft->mSchemaStale)
        {
            draft->mStatusIsError = true;
            draft->mStatus = "The current source or reflection could not be accepted; fix it before saving.";
            return;
        }
        if (context.mMaterials != nullptr && context.mMaterials->IsActiveAsset(draft->mLogicalPath) &&
            !draft->mPreviewError.empty())
        {
            draft->mStatusIsError = true;
            draft->mStatus = "Fix the preview validation error before saving.";
            return;
        }

        const std::optional<std::filesystem::path> source =
            MaterialSourceFile(context.mSourceAssetRoot, context.mAssetIdRoot, draft->mLogicalPath);
        if (!source)
        {
            draft->mStatusIsError = true;
            draft->mStatus = "The material is not under the project's assets root.";
            return;
        }

        std::error_code existsError;
        if (!std::filesystem::is_regular_file(*source, existsError))
        {
            draft->mStatusIsError = true;
            draft->mStatus = "The source material file no longer exists: " + source->string();
            return;
        }

        const MaterialDraftSavePatch patch = MaterialDraftSavePatchFor(*draft);
        std::string error;
        if (!patch.Empty() && !SaveMaterialRecipe(*source, patch.mParametersJson, patch.mTextures, &error))
        {
            draft->mStatusIsError = true;
            draft->mStatus = error;
            return;
        }

        const bool retriedUnchanged = patch.Empty();
        draft->mSourceSignature = FileSignature(*source);
        mSourceSignature = draft->mSourceSignature;
        const bool active = context.mMaterials != nullptr &&
            context.mMaterials->IsActiveAsset(draft->mLogicalPath);
        const uint64_t generation = context.mMaterials != nullptr ? context.mMaterials->Generation() : 0;
        const uint64_t request = context.mReimportMaterial ? context.mReimportMaterial(draft->mLogicalPath) : 0;
        MaterialDraftMarkSaved(*draft, request, generation, active);
        if (retriedUnchanged)
            draft->mStatus = "Retrying the last written material values.";
        if (!context.mReimportMaterial)
        {
            draft->mCooking = false;
            draft->mStatusIsError = true;
            draft->mStatus = "Saved to the recipe, but no importer is available to cook it.";
        }
    }

    void MaterialEditor::RevertDraft(bool editing)
    {
        MaterialDraft *draft = ActiveDraft();
        if (!editing || draft == nullptr || !MaterialDraftNeedsSave(*draft))
            return;
        MaterialDraftRevert(*draft);
    }
}
