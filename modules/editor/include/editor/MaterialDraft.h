/**
 * @file MaterialDraft.h
 * @author Jackey Cen
 * @brief Per-asset material values, their recipe baseline and their cook state.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/MaterialRecipe.h>

#include <renderer/MaterialReflection.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    struct MaterialDraftField
    {
        MaterialFieldLayout mLayout;
        std::array<float, 4> mComponents{};
        uint32_t mUnsigned = 0;
        int32_t mSigned = 0;
        bool mBool = false;

        bool operator==(const MaterialDraftField &other) const
        {
            return mLayout.name == other.mLayout.name && mLayout.kind == other.mLayout.kind &&
                mLayout.offset == other.mLayout.offset && mLayout.size == other.mLayout.size &&
                mComponents == other.mComponents && mUnsigned == other.mUnsigned && mSigned == other.mSigned &&
                mBool == other.mBool;
        }
    };

    struct MaterialTextureSource
    {
        std::string mRole;
        std::string mColorSpace;
        std::string mPath;
        std::string mBaselinePath;

        bool operator==(const MaterialTextureSource &) const = default;
    };

    // One reflected TextureRef field and the authored source roles that feed it.
    struct MaterialTextureControl
    {
        std::string mField;
        std::string mRole;
        std::vector<std::string> mRecipeRoles;
        bool mEditable = false;
        std::string mReason;
    };

    struct MaterialDraft
    {
        std::string mIdentity;
        std::string mLogicalPath;
        std::string mProjectKey;
        std::string mFrontend;
        std::string mShaderType;
        std::vector<std::string> mFrontends;
        std::string mSchemaRevision;
        std::vector<std::string> mSchemaMessages;
        std::vector<MaterialDraftField> mFields;
        std::vector<MaterialDraftField> mBaselineFields;
        std::vector<MaterialTextureControl> mTextureControls;
        std::vector<MaterialTextureSource> mTextures;
        std::vector<std::string> mUnsupported;
        // The values written by the last successful Save, or the loaded values
        // while nothing is cooking.
        std::vector<MaterialDraftField> mSavedFields;
        std::vector<MaterialTextureSource> mSavedTextures;
        bool mCooking = false;
        bool mSchemaStale = false;
        bool mApplyFailed = false;
        uint64_t mSavedRequestSequence = 0;
        uint64_t mSavedGeneration = 0;
        bool mSavedActive = false;
        uint64_t mFailedSourceSignature = 0;
        uint64_t mGeneration = 0;
        uint64_t mSourceSignature = 0;
        uint64_t mAcceptedRevision = 0;
        std::string mStatus;
        bool mStatusIsError = false;
        std::string mPreviewError;
        std::string mPreviewedParameters;
        std::vector<std::string> mPreviewedFields;
        uint64_t mPreviewedGeneration = 0;
        bool mPreviewedActive = false;
    };

    struct MaterialDraftRefresh
    {
        std::vector<std::string> mMessages;
    };

    struct MaterialDraftSavePatch
    {
        std::string mParametersJson = "{}";
        std::map<std::string, std::string> mTextures;

        bool Empty() const { return mParametersJson == "{}" && mTextures.empty(); }
    };

    enum class MaterialCookOutcome
    {
        Waiting,
        Applied,
        Failed,
    };

    enum class MaterialAcceptedRefresh
    {
        None,
        SchemaOnly,
        Accepted,
    };

    uint64_t MaterialAcceptedRevision(std::string_view shaderType, std::string_view parametersJson,
        const std::vector<MaterialRecipeTexture> &textures, const std::vector<std::string> &frontends);
    MaterialAcceptedRefresh DecideMaterialAcceptedRefresh(const MaterialDraft &draft, uint64_t acceptedRevision,
        std::string_view schemaRevision);

    class MaterialDraftStore
    {
    public:
        MaterialDraft &Put(MaterialDraft draft);
        MaterialDraft *Find(std::string_view identity);
        const MaterialDraft *Find(std::string_view identity) const;
        bool HasDirty() const;
        bool HasApplyFailed() const;
        bool HasCooking() const;
        std::vector<MaterialDraft *> All();
        std::size_t Size() const { return mDrafts.size(); }

    private:
        std::map<std::string, MaterialDraft> mDrafts;
    };

    // A normalized project identity and logical asset path. Two projects never
    // share a draft, and neither do two spellings of the same path.
    std::optional<std::string> MaterialDraftIdentity(std::string_view projectKey, std::string_view logicalPath);

    std::optional<MaterialDraft> BuildMaterialDraft(std::string identity, std::string logicalPath,
        std::string projectKey, std::string frontend, const MaterialRecipe &recipe, const MaterialLayout &layout,
        std::string *error = nullptr);

    // Refreshes a draft against a new accepted recipe and reflected layout.
    // Compatible unsaved values survive by name and kind; added fields take the
    // new baseline, removed/type-changed dirty fields are reported explicitly.
    // The frontend must identify the supplied reflected layout; both change together on success.
    std::optional<MaterialDraftRefresh> RefreshMaterialDraft(MaterialDraft &draft, const MaterialRecipe &recipe,
        const MaterialLayout &layout, std::string_view frontend, std::string_view schemaRevision,
        std::string *error = nullptr);

    std::optional<MaterialDraftField> MaterialDraftFieldFromJson(const MaterialFieldLayout &layout,
        std::string_view valueJson, std::string *error = nullptr);
    std::string MaterialDraftFieldJson(const MaterialDraftField &field);
    std::string MaterialDraftParametersJson(const MaterialDraft &draft);
    std::string MaterialDraftDirtyParametersJson(const MaterialDraft &draft);
    std::string MaterialDraftBaselineParametersJson(const MaterialDraft &draft);
    std::map<std::string, std::string> MaterialDraftDirtyTexturePaths(const MaterialDraft &draft);
    std::map<std::string, std::string> MaterialDraftBaselineTexturePaths(const MaterialDraft &draft);
    bool MaterialDraftDirty(const MaterialDraft &draft);
    bool MaterialDraftFinite(const MaterialDraft &draft);
    // True while the draft differs from the accepted baseline or the source has
    // a written value the runtime has not accepted.
    bool MaterialDraftNeedsSave(const MaterialDraft &draft);
    // Normal edits patch the accepted baseline; a failed apply patches the last
    // written source values, so restoring accepted values is an explicit repair.
    MaterialDraftSavePatch MaterialDraftSavePatchFor(const MaterialDraft &draft);

    void MaterialDraftRevert(MaterialDraft &draft);
    void MaterialDraftMarkSaved(MaterialDraft &draft, uint64_t requestSequence, uint64_t generation, bool active);
    bool MaterialDraftCooked(MaterialDraft &draft, bool success, const std::string &diagnostic);

    MaterialCookOutcome ResolveMaterialCook(const MaterialDraft &draft, bool hasModel, bool failed,
        const std::string &diagnostic, bool requiresRendererApply, std::optional<uint64_t> cookRevision,
        std::optional<uint64_t> failureRevision, uint64_t generation, std::string *message = nullptr);

    // The partial numeric overlay to publish: dirty fields plus the baseline
    // values of fields this draft previewed before. Empty when the record is
    // already current. Play and an inactive material produce nullopt.
    std::optional<std::string> MaterialDraftPreviewParameters(MaterialDraft &draft, bool editing, bool active,
        uint64_t generation);
    void MaterialDraftPreviewed(MaterialDraft &draft, uint64_t generation, std::string_view overlayJson);
    void MaterialDraftPreviewRejected(MaterialDraft &draft, uint64_t generation, std::string_view overlayJson,
        std::string error);
    void MaterialDraftResetPreview(MaterialDraft &draft);

    bool MaterialFieldIsColor(std::string_view name);
    std::vector<std::string> MaterialTextureRecipeRoles(std::string_view role);
}
