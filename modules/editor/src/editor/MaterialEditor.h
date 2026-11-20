/**
 * @file MaterialEditor.h
 * @author Jackey Cen
 * @brief The reflected Material panel, its per-asset drafts and cook state.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/MaterialDraft.h>
#include <editor/panels/AssetImportPanel.h>
#include <renderer/MaterialReflection.h>
#include <renderer/SurfaceMaterialCatalog.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    class SurfaceMaterialLibrary;

    // The cooked payload the runtime actually accepts. Raw source values are
    // not used as a baseline before a cook applies.
    struct MaterialCookedView
    {
        std::string shaderType;
        std::string parametersJson;
        std::vector<MaterialRecipeTexture> textures;
        std::vector<std::string> frontends;
        uint32_t resolution = 2048;
    };

    struct MaterialEditorContext
    {
        SurfaceMaterialLibrary *mMaterials = nullptr;
        const SurfaceMaterialCatalog *mCatalog = nullptr;
        std::function<uint64_t(std::string_view, bool)> mReimportMaterial;
        std::function<std::optional<uint64_t>(std::string_view)> mMaterialCookRevision;
        std::function<std::optional<uint64_t>(std::string_view)> mMaterialCookFailureRevision;
        std::function<bool(std::string_view, MaterialCookedView &, std::string &)> mLoadCooked;
        std::filesystem::path mSourceAssetRoot;
        std::string mAssetIdRoot;
        std::string mProjectKey;
    };

    class MaterialEditor
    {
    public:
        // The normalized logical asset path selected in the content browser,
        // or nullopt for none.
        void Select(std::optional<std::string> logicalPath);
        void Draw(bool *visible, bool editing, const AssetImportView &import, const MaterialEditorContext &context);
        bool HasUnsavedDrafts() const;

    private:
        enum class TargetState
        {
            None,
            Loading,
            Invalid,
            Ready,
        };

        void SyncTarget(const AssetImportView &import, const MaterialEditorContext &context);
        bool LoadTarget(const AssetImportView &import, const MaterialEditorContext &context);
        void RefreshDrafts(const MaterialEditorContext &context);
        void PreviewDrafts(bool editing, const MaterialEditorContext &context);
        void UpdateCooks(const AssetImportView &import, const MaterialEditorContext &context);
        void DrawDraft(bool editing, const AssetImportView &import, const MaterialEditorContext &context);
        void DrawControls(bool editing);
        void SaveDraft(bool editing, const MaterialEditorContext &context);
        void RevertDraft(bool editing);
        MaterialDraft *ActiveDraft();

        MaterialDraftStore mDrafts;
        std::optional<std::string> mSelection;
        std::string mActiveIdentity;
        std::string mLoadError;
        // The source exists but has never been cooked, so Import is offered.
        bool mNotImported = false;
        MaterialRecipe mRecipe;
        MaterialLayout mLayout;
        std::vector<std::string> mFrontends;
        std::string mFrontendOverride;
        uint64_t mSourceSignature = 0;
        std::chrono::steady_clock::time_point mLastLoadAttempt{};
        TargetState mState = TargetState::None;
    };
}
