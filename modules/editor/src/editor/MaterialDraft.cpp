/**
 * @file MaterialDraft.cpp
 * @author Jackey Cen
 * @brief Per-asset material values, their recipe baseline and their cook state.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/MaterialDraft.h>

#include <assets/AssetPath.h>
#include <renderer/SurfaceMaterialTexture.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace mir
{
    namespace
    {
        using Json = nlohmann::json;

        void SetError(std::string *error, std::string message)
        {
            if (error != nullptr)
                *error = std::move(message);
        }

        uint32_t ComponentCount(MaterialFieldKind kind)
        {
            switch (kind)
            {
            case MaterialFieldKind::Float2:
                return 2;
            case MaterialFieldKind::Float3:
                return 3;
            case MaterialFieldKind::Float4:
                return 4;
            default:
                return 1;
            }
        }

        std::optional<float> ReadFloat(const Json &value, std::string_view name, std::string *error)
        {
            if (!value.is_number())
            {
                SetError(error, "material parameter '" + std::string(name) + "' must be a number");
                return std::nullopt;
            }
            const double wide = value.get<double>();
            if (!std::isfinite(wide) || std::abs(wide) > std::numeric_limits<float>::max())
            {
                SetError(error, "material parameter '" + std::string(name) + "' is not a finite float");
                return std::nullopt;
            }
            return static_cast<float>(wide);
        }

        Json FieldJsonValue(const MaterialDraftField &field)
        {
            switch (field.mLayout.kind)
            {
            case MaterialFieldKind::Float:
                return field.mComponents[0];
            case MaterialFieldKind::Float2:
            {
                Json value = Json::array();
                value.push_back(field.mComponents[0]);
                value.push_back(field.mComponents[1]);
                return value;
            }
            case MaterialFieldKind::Float3:
            {
                Json value = Json::array();
                for (uint32_t i = 0; i < 3; ++i)
                    value.push_back(field.mComponents[i]);
                return value;
            }
            case MaterialFieldKind::Float4:
            {
                Json value = Json::array();
                for (uint32_t i = 0; i < 4; ++i)
                    value.push_back(field.mComponents[i]);
                return value;
            }
            case MaterialFieldKind::Uint:
                return field.mUnsigned;
            case MaterialFieldKind::Int:
                return field.mSigned;
            case MaterialFieldKind::Bool:
                return field.mBool;
            case MaterialFieldKind::TextureRef:
                break;
            }
            return Json();
        }

        const MaterialDraftField *BaselineField(const MaterialDraft &draft, std::string_view name)
        {
            const auto found = std::find_if(draft.mBaselineFields.begin(), draft.mBaselineFields.end(),
                [&](const MaterialDraftField &field) { return field.mLayout.name == name; });
            return found != draft.mBaselineFields.end() ? &*found : nullptr;
        }

        const MaterialTextureSource *TextureSource(const std::vector<MaterialTextureSource> &textures,
            std::string_view role)
        {
            const auto found = std::find_if(textures.begin(), textures.end(),
                [&](const MaterialTextureSource &texture) { return texture.mRole == role; });
            return found != textures.end() ? &*found : nullptr;
        }

        std::string RoleName(SurfaceTextureRole role)
        {
            switch (role)
            {
            case SurfaceTextureRole::Albedo:
                return "albedo";
            case SurfaceTextureRole::Normal:
                return "normal";
            case SurfaceTextureRole::Surface:
                return "surface";
            case SurfaceTextureRole::Count:
                break;
            }
            return {};
        }

        bool ContainsLower(std::string_view text, std::string_view token)
        {
            std::string lower(text);
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lower.find(token) != std::string::npos;
        }
    } // namespace

    MaterialDraft &MaterialDraftStore::Put(MaterialDraft draft)
    {
        const std::string identity = draft.mIdentity;
        return mDrafts.insert_or_assign(identity, std::move(draft)).first->second;
    }

    MaterialDraft *MaterialDraftStore::Find(std::string_view identity)
    {
        const auto found = mDrafts.find(std::string(identity));
        return found != mDrafts.end() ? &found->second : nullptr;
    }

    const MaterialDraft *MaterialDraftStore::Find(std::string_view identity) const
    {
        const auto found = mDrafts.find(std::string(identity));
        return found != mDrafts.end() ? &found->second : nullptr;
    }

    bool MaterialDraftStore::HasDirty() const
    {
        return std::any_of(mDrafts.begin(), mDrafts.end(),
            [](const auto &entry) { return MaterialDraftDirty(entry.second); });
    }

    bool MaterialDraftStore::HasApplyFailed() const
    {
        return std::any_of(mDrafts.begin(), mDrafts.end(),
            [](const auto &entry) { return entry.second.mApplyFailed; });
    }

    bool MaterialDraftStore::HasCooking() const
    {
        return std::any_of(mDrafts.begin(), mDrafts.end(), [](const auto &entry) { return entry.second.mCooking; });
    }

    std::vector<MaterialDraft *> MaterialDraftStore::All()
    {
        std::vector<MaterialDraft *> drafts;
        drafts.reserve(mDrafts.size());
        for (auto &[identity, draft] : mDrafts)
            drafts.push_back(&draft);
        return drafts;
    }

    std::optional<std::string> MaterialDraftIdentity(std::string_view projectKey, std::string_view logicalPath)
    {
        const AssetPathResult project = NormalizeAssetPath(projectKey);
        const AssetPathResult logical = NormalizeAssetPath(logicalPath);
        if (!project.Ok() || !logical.Ok())
            return std::nullopt;
        return project.path + "|" + logical.path;
    }

    uint64_t MaterialAcceptedRevision(std::string_view shaderType, std::string_view parametersJson,
        const std::vector<MaterialRecipeTexture> &textures, const std::vector<std::string> &frontends)
    {
        uint64_t hash = 1469598103934665603ull;
        const auto mix = [&](std::string_view bytes)
        {
            for (const unsigned char byte : bytes)
            {
                hash ^= byte;
                hash *= 1099511628211ull;
            }
        };
        mix(shaderType);
        mix(parametersJson);
        for (const std::string &frontend : frontends)
        {
            mix("|");
            mix(frontend);
        }
        for (const MaterialRecipeTexture &texture : textures)
        {
            mix("|");
            mix(texture.role);
            mix(":");
            mix(texture.path);
            mix(":");
            mix(texture.colorSpace);
        }
        return hash;
    }

    MaterialAcceptedRefresh DecideMaterialAcceptedRefresh(const MaterialDraft &draft, uint64_t acceptedRevision,
        std::string_view schemaRevision)
    {
        if (schemaRevision != draft.mSchemaRevision)
            return MaterialAcceptedRefresh::SchemaOnly;
        if (acceptedRevision != draft.mAcceptedRevision)
            return MaterialAcceptedRefresh::Accepted;
        return MaterialAcceptedRefresh::None;
    }

    std::optional<MaterialDraftField> MaterialDraftFieldFromJson(const MaterialFieldLayout &layout,
        std::string_view valueJson, std::string *error)
    {
        Json value;
        try
        {
            value = Json::parse(valueJson);
        }
        catch (const Json::exception &exception)
        {
            SetError(error, std::string("material parameter '") + layout.name + "' is not JSON: " + exception.what());
            return std::nullopt;
        }

        MaterialDraftField field;
        field.mLayout = layout;
        switch (layout.kind)
        {
        case MaterialFieldKind::Float:
        case MaterialFieldKind::Float2:
        case MaterialFieldKind::Float3:
        case MaterialFieldKind::Float4:
        {
            const uint32_t count = ComponentCount(layout.kind);
            if (count == 1)
            {
                const std::optional<float> number = ReadFloat(value, layout.name, error);
                if (!number)
                    return std::nullopt;
                field.mComponents[0] = *number;
                return field;
            }
            if (!value.is_array() || value.size() != count)
            {
                SetError(error, "material parameter '" + layout.name + "' must have " + std::to_string(count) +
                        " components");
                return std::nullopt;
            }
            for (uint32_t i = 0; i < count; ++i)
            {
                const std::optional<float> number = ReadFloat(value[i], layout.name, error);
                if (!number)
                    return std::nullopt;
                field.mComponents[i] = *number;
            }
            return field;
        }
        case MaterialFieldKind::Uint:
            if (!value.is_number_unsigned() || value.get<uint64_t>() > UINT32_MAX)
            {
                SetError(error, "material parameter '" + layout.name + "' must be an unsigned 32-bit integer");
                return std::nullopt;
            }
            field.mUnsigned = value.get<uint32_t>();
            return field;
        case MaterialFieldKind::Int:
            if (!value.is_number_integer())
            {
                SetError(error, "material parameter '" + layout.name + "' must be an integer");
                return std::nullopt;
            }
            if ((value.is_number_unsigned() && value.get<uint64_t>() > INT32_MAX) ||
                (!value.is_number_unsigned() &&
                    (value.get<int64_t>() < INT32_MIN || value.get<int64_t>() > INT32_MAX)))
            {
                SetError(error, "material parameter '" + layout.name + "' is out of int32 range");
                return std::nullopt;
            }
            field.mSigned = value.get<int32_t>();
            return field;
        case MaterialFieldKind::Bool:
            if (!value.is_boolean())
            {
                SetError(error, "material parameter '" + layout.name + "' must be a boolean");
                return std::nullopt;
            }
            field.mBool = value.get<bool>();
            return field;
        case MaterialFieldKind::TextureRef:
            SetError(error, "material texture '" + layout.name + "' is edited through the source recipe");
            return std::nullopt;
        }
        SetError(error, "material parameter '" + layout.name + "' has an unsupported field kind");
        return std::nullopt;
    }

    std::string MaterialDraftFieldJson(const MaterialDraftField &field)
    {
        return FieldJsonValue(field).dump();
    }

    std::optional<MaterialDraft> BuildMaterialDraft(std::string identity, std::string logicalPath,
        std::string projectKey, std::string frontend, const MaterialRecipe &recipe, const MaterialLayout &layout,
        std::string *error)
    {
        Json parameters;
        try
        {
            parameters = Json::parse(recipe.parametersJson);
        }
        catch (const Json::exception &exception)
        {
            SetError(error, std::string("the material parameters are not JSON: ") + exception.what());
            return std::nullopt;
        }
        if (!parameters.is_object())
        {
            SetError(error, "the material parameters must be an object");
            return std::nullopt;
        }

        MaterialDraft draft;
        draft.mIdentity = std::move(identity);
        draft.mLogicalPath = std::move(logicalPath);
        draft.mProjectKey = std::move(projectKey);
        draft.mFrontend = std::move(frontend);
        draft.mShaderType = recipe.shaderType;
        draft.mFrontends = recipe.frontends;
        for (const MaterialRecipeTexture &texture : recipe.textures)
            draft.mTextures.push_back({texture.role, texture.colorSpace, texture.path, texture.path});

        std::vector<std::string> usedParameters;
        for (const MaterialFieldLayout &field : layout.fields)
        {
            usedParameters.push_back(field.name);
            if (field.kind == MaterialFieldKind::TextureRef)
            {
                const std::optional<SurfaceTextureRole> role = SurfaceTextureRoleForField(field.name);
                MaterialTextureControl control;
                control.mField = field.name;
                if (!role)
                {
                    control.mReason = "no packed source role; the resolved texture row is preserved";
                    draft.mUnsupported.push_back("texture field '" + field.name + "' is not editable");
                    draft.mTextureControls.push_back(std::move(control));
                    continue;
                }
                control.mRole = RoleName(*role);
                control.mRecipeRoles = MaterialTextureRecipeRoles(control.mRole);
                std::erase_if(control.mRecipeRoles, [&](const std::string &recipeRole)
                    { return TextureSource(draft.mTextures, recipeRole) == nullptr; });
                control.mEditable = !control.mRecipeRoles.empty();
                if (!control.mEditable)
                    control.mReason = "the recipe authors no source texture for this role";
                draft.mTextureControls.push_back(std::move(control));
                continue;
            }

            const auto value = parameters.find(field.name);
            if (value == parameters.end())
            {
                SetError(error, "material parameter '" + field.name + "' is missing from the recipe");
                return std::nullopt;
            }
            const std::optional<MaterialDraftField> parsed = MaterialDraftFieldFromJson(field, value->dump(), error);
            if (!parsed)
                return std::nullopt;
            draft.mFields.push_back(*parsed);
        }

        for (const auto &[name, value] : parameters.items())
            if (std::find(usedParameters.begin(), usedParameters.end(), name) == usedParameters.end())
                draft.mUnsupported.push_back("parameter '" + name + "' is not reflected; the recipe value is preserved");

        for (const MaterialTextureSource &texture : draft.mTextures)
        {
            const bool used = std::any_of(draft.mTextureControls.begin(), draft.mTextureControls.end(),
                [&](const MaterialTextureControl &control)
                {
                    return std::find(control.mRecipeRoles.begin(), control.mRecipeRoles.end(), texture.mRole) !=
                        control.mRecipeRoles.end();
                });
            if (!used)
                draft.mUnsupported.push_back("texture '" + texture.mRole + "' is not reflected; the recipe value is preserved");
        }

        draft.mBaselineFields = draft.mFields;
        return draft;
    }

    std::optional<MaterialDraftRefresh> RefreshMaterialDraft(MaterialDraft &draft, const MaterialRecipe &recipe,
        const MaterialLayout &layout, std::string_view frontend, std::string_view schemaRevision, std::string *error)
    {
        std::optional<MaterialDraft> fresh = BuildMaterialDraft(draft.mIdentity, draft.mLogicalPath,
            draft.mProjectKey, std::string(frontend), recipe, layout, error);
        if (!fresh)
            return std::nullopt;

        MaterialDraftRefresh result;
        const auto oldField = [&](std::string_view name) -> const MaterialDraftField *
        {
            const auto found = std::find_if(draft.mFields.begin(), draft.mFields.end(),
                [&](const MaterialDraftField &field) { return field.mLayout.name == name; });
            return found != draft.mFields.end() ? &*found : nullptr;
        };
        const auto dirty = [&](const MaterialDraftField &field)
        {
            const MaterialDraftField *baseline = BaselineField(draft, field.mLayout.name);
            return baseline == nullptr || !(*baseline == field);
        };

        for (MaterialDraftField &field : fresh->mFields)
        {
            const MaterialDraftField *old = oldField(field.mLayout.name);
            if (old == nullptr)
                continue;
            if (old->mLayout.kind != field.mLayout.kind)
            {
                result.mMessages.push_back("field '" + field.mLayout.name +
                    "' changed type; its unsaved value was discarded");
                continue;
            }
            if (dirty(*old))
            {
                MaterialDraftField value = *old;
                value.mLayout = field.mLayout;
                field = value;
            }
        }

        for (const MaterialDraftField &old : draft.mFields)
        {
            const auto current = std::find_if(fresh->mFields.begin(), fresh->mFields.end(),
                [&](const MaterialDraftField &field) { return field.mLayout.name == old.mLayout.name; });
            if (current == fresh->mFields.end() && dirty(old))
                result.mMessages.push_back("field '" + old.mLayout.name + "' was removed; its unsaved value was discarded");
        }

        for (MaterialTextureSource &texture : fresh->mTextures)
        {
            const MaterialTextureSource *old = TextureSource(draft.mTextures, texture.mRole);
            if (old != nullptr && old->mPath != old->mBaselinePath)
                texture.mPath = old->mPath;
        }
        for (const MaterialTextureSource &old : draft.mTextures)
        {
            const MaterialTextureSource *current = TextureSource(fresh->mTextures, old.mRole);
            if (current == nullptr && old.mPath != old.mBaselinePath)
                result.mMessages.push_back("texture '" + old.mRole + "' was removed; its unsaved path was discarded");
        }

        fresh->mSchemaRevision = std::string(schemaRevision);
        fresh->mSchemaMessages = result.mMessages;
        fresh->mSchemaStale = false;
        fresh->mStatusIsError = false;
        fresh->mStatus = result.mMessages.empty() ? "Refreshed from the accepted schema."
                                                   : "Refreshed; some unsaved fields were incompatible.";
        draft = std::move(*fresh);
        return result;
    }

    std::string MaterialDraftParametersJson(const MaterialDraft &draft)
    {
        Json parameters = Json::object();
        for (const MaterialDraftField &field : draft.mFields)
            parameters[field.mLayout.name] = FieldJsonValue(field);
        return parameters.dump();
    }

    std::string MaterialDraftDirtyParametersJson(const MaterialDraft &draft)
    {
        Json parameters = Json::object();
        for (const MaterialDraftField &field : draft.mFields)
        {
            const MaterialDraftField *baseline = BaselineField(draft, field.mLayout.name);
            if (baseline == nullptr || !(*baseline == field))
                parameters[field.mLayout.name] = FieldJsonValue(field);
        }
        return parameters.dump();
    }

    std::string MaterialDraftBaselineParametersJson(const MaterialDraft &draft)
    {
        Json parameters = Json::object();
        for (const MaterialDraftField &baseline : draft.mBaselineFields)
        {
            const auto current = std::find_if(draft.mFields.begin(), draft.mFields.end(),
                [&](const MaterialDraftField &field) { return field.mLayout.name == baseline.mLayout.name; });
            if (current == draft.mFields.end() || !(*current == baseline))
                parameters[baseline.mLayout.name] = FieldJsonValue(baseline);
        }
        return parameters.dump();
    }

    std::map<std::string, std::string> MaterialDraftDirtyTexturePaths(const MaterialDraft &draft)
    {
        std::map<std::string, std::string> paths;
        for (const MaterialTextureSource &texture : draft.mTextures)
            if (texture.mPath != texture.mBaselinePath)
                paths[texture.mRole] = texture.mPath;
        return paths;
    }

    std::map<std::string, std::string> MaterialDraftBaselineTexturePaths(const MaterialDraft &draft)
    {
        std::map<std::string, std::string> paths;
        for (const MaterialTextureSource &texture : draft.mTextures)
            if (texture.mPath != texture.mBaselinePath)
                paths[texture.mRole] = texture.mBaselinePath;
        return paths;
    }

    bool MaterialDraftDirty(const MaterialDraft &draft)
    {
        if (!MaterialDraftDirtyTexturePaths(draft).empty())
            return true;
        if (draft.mBaselineFields.size() != draft.mFields.size())
            return true;
        return std::any_of(draft.mFields.begin(), draft.mFields.end(),
            [&](const MaterialDraftField &field)
            {
                const MaterialDraftField *baseline = BaselineField(draft, field.mLayout.name);
                return baseline == nullptr || !(*baseline == field);
            });
    }

    bool MaterialDraftNeedsSave(const MaterialDraft &draft)
    {
        return MaterialDraftDirty(draft) || draft.mApplyFailed;
    }

    MaterialDraftSavePatch MaterialDraftSavePatchFor(const MaterialDraft &draft)
    {
        MaterialDraftSavePatch patch;
        if (!draft.mApplyFailed)
        {
            patch.mParametersJson = MaterialDraftDirtyParametersJson(draft);
            patch.mTextures = MaterialDraftDirtyTexturePaths(draft);
            return patch;
        }

        Json parameters = Json::object();
        for (const MaterialDraftField &field : draft.mFields)
        {
            const auto saved = std::find_if(draft.mSavedFields.begin(), draft.mSavedFields.end(),
                [&](const MaterialDraftField &candidate) { return candidate.mLayout.name == field.mLayout.name; });
            if (saved == draft.mSavedFields.end() || !(*saved == field))
                parameters[field.mLayout.name] = FieldJsonValue(field);
        }
        patch.mParametersJson = parameters.dump();
        for (const MaterialTextureSource &texture : draft.mTextures)
        {
            const MaterialTextureSource *saved = TextureSource(draft.mSavedTextures, texture.mRole);
            if (saved == nullptr || saved->mPath != texture.mPath)
                patch.mTextures[texture.mRole] = texture.mPath;
        }
        return patch;
    }
    bool MaterialDraftFinite(const MaterialDraft &draft)
    {
        for (const MaterialDraftField &field : draft.mFields)
        {
            switch (field.mLayout.kind)
            {
            case MaterialFieldKind::Float:
            case MaterialFieldKind::Float2:
            case MaterialFieldKind::Float3:
            case MaterialFieldKind::Float4:
                for (uint32_t i = 0; i < ComponentCount(field.mLayout.kind); ++i)
                    if (!std::isfinite(field.mComponents[i]))
                        return false;
                break;
            case MaterialFieldKind::Uint:
            case MaterialFieldKind::Int:
            case MaterialFieldKind::Bool:
            case MaterialFieldKind::TextureRef:
                break;
            }
        }
        return true;
    }

    void MaterialDraftRevert(MaterialDraft &draft)
    {
        Json restore = Json::object();
        for (const MaterialDraftField &field : draft.mFields)
        {
            const MaterialDraftField *baseline = BaselineField(draft, field.mLayout.name);
            if (baseline == nullptr || (*baseline == field))
                continue;
            restore[field.mLayout.name] = FieldJsonValue(*baseline);
        }

        draft.mFields = draft.mBaselineFields;
        for (MaterialTextureSource &texture : draft.mTextures)
            texture.mPath = texture.mBaselinePath;
        draft.mStatusIsError = false;
        draft.mStatus = draft.mApplyFailed
            ? "Reverted in memory; the source still has an unapplied write. Save to repair it."
            : "Reverted to the accepted values.";
        if (restore.empty())
            return;

        // A generation or schema refresh can clear the preview bookkeeping before
        // the next preview tick, so the baseline values travel with the draft.
        draft.mPendingRestoreJson = restore.dump();
        draft.mPreviewedActive = false;
        draft.mPreviewedParameters.clear();
    }

    void MaterialDraftMarkSaved(MaterialDraft &draft, uint64_t requestSequence, uint64_t generation, bool active)
    {
        draft.mSavedFields = draft.mFields;
        draft.mSavedTextures = draft.mTextures;
        draft.mSavedRequestSequence = requestSequence;
        draft.mSavedGeneration = generation;
        draft.mSavedActive = active;
        draft.mCooking = true;
        draft.mApplyFailed = false;
        draft.mStatusIsError = false;
        draft.mStatus = "Saved; waiting for the material cook.";
    }

    bool MaterialDraftCooked(MaterialDraft &draft, bool success, const std::string &diagnostic)
    {
        draft.mCooking = false;
        if (!success)
        {
            draft.mApplyFailed = true;
            draft.mFailedSourceSignature = draft.mSourceSignature;
            draft.mStatusIsError = true;
            draft.mStatus = diagnostic.empty() ? "The material cook failed; the last good material is unchanged."
                                               : diagnostic;
            return false;
        }

        draft.mApplyFailed = false;
        if (!draft.mSavedFields.empty())
            draft.mBaselineFields = draft.mSavedFields;
        for (MaterialTextureSource &texture : draft.mTextures)
        {
            const MaterialTextureSource *saved = TextureSource(draft.mSavedTextures, texture.mRole);
            if (saved != nullptr)
                texture.mBaselinePath = saved->mPath;
        }
        draft.mStatusIsError = false;
        draft.mStatus = MaterialDraftDirty(draft) ? "Saved; newer edits are not saved yet." : "Saved and applied.";
        return true;
    }

    MaterialCookOutcome ResolveMaterialCook(const MaterialDraft &draft, bool hasModel, bool failed,
        const std::string &diagnostic, bool requiresRendererApply, std::optional<uint64_t> cookRevision,
        std::optional<uint64_t> failureRevision, uint64_t generation, std::string *message)
    {
        const auto setMessage = [&](std::string value)
        {
            if (message != nullptr)
                *message = std::move(value);
        };
        if (!hasModel)
        {
            setMessage("Waiting for the material source to be imported.");
            return MaterialCookOutcome::Waiting;
        }
        if (failed)
        {
            if (failureRevision && *failureRevision < draft.mSavedRequestSequence)
            {
                setMessage("Waiting for this save's material cook result.");
                return MaterialCookOutcome::Waiting;
            }
            setMessage(diagnostic.empty() ? "The material cook failed; the last good material is unchanged."
                                          : diagnostic);
            return MaterialCookOutcome::Failed;
        }
        if (!cookRevision || *cookRevision < draft.mSavedRequestSequence)
        {
            setMessage("Waiting for this save's material cook.");
            return MaterialCookOutcome::Waiting;
        }
        if (requiresRendererApply && draft.mSavedActive && generation <= draft.mSavedGeneration)
        {
            setMessage("Waiting for the renderer to apply the material cook.");
            return MaterialCookOutcome::Waiting;
        }
        setMessage({});
        return MaterialCookOutcome::Applied;
    }

    std::string MaterialDraftPreviewOverlay(const MaterialDraft &draft)
    {
        Json overlay = Json::object();
        if (!draft.mPendingRestoreJson.empty())
        {
            const Json restore = Json::parse(draft.mPendingRestoreJson);
            if (restore.is_object())
                for (const auto &[name, value] : restore.items())
                    overlay[name] = value;
        }
        for (const MaterialDraftField &field : draft.mFields)
        {
            const MaterialDraftField *baseline = BaselineField(draft, field.mLayout.name);
            const bool dirty = baseline == nullptr || !(*baseline == field);
            const bool previouslyPreviewed =
                std::find(draft.mPreviewedFields.begin(), draft.mPreviewedFields.end(), field.mLayout.name) !=
                draft.mPreviewedFields.end();
            if (dirty)
                overlay[field.mLayout.name] = FieldJsonValue(field);
            else if (previouslyPreviewed && baseline != nullptr)
                overlay[field.mLayout.name] = FieldJsonValue(*baseline);
        }
        return overlay.dump();
    }

    std::optional<std::string> MaterialDraftPreviewParameters(MaterialDraft &draft, bool editing, bool active,
        uint64_t generation)
    {
        if (!active || !editing)
            return std::nullopt;
        if (draft.mPreviewedGeneration != generation)
        {
            draft.mPreviewedFields.clear();
            draft.mPreviewedParameters.clear();
            draft.mPreviewedGeneration = generation;
            draft.mPreviewedActive = false;
        }
        const std::string overlay = MaterialDraftPreviewOverlay(draft);
        if (overlay == "{}" || (draft.mPreviewedActive && draft.mPreviewedParameters == overlay))
            return std::nullopt;
        return overlay;
    }

    void MaterialDraftPreviewed(MaterialDraft &draft, uint64_t generation, std::string_view overlayJson)
    {
        draft.mPreviewedActive = true;
        draft.mPreviewedGeneration = generation;
        draft.mPreviewedParameters = std::string(overlayJson);
        draft.mPreviewedFields.clear();
        for (const MaterialDraftField &field : draft.mFields)
        {
            const MaterialDraftField *baseline = BaselineField(draft, field.mLayout.name);
            if (baseline == nullptr || !(*baseline == field))
                draft.mPreviewedFields.push_back(field.mLayout.name);
        }
        draft.mPreviewError.clear();
        draft.mPendingRestoreJson.clear();
    }

    void MaterialDraftPreviewRejected(MaterialDraft &draft, uint64_t generation, std::string_view overlayJson,
        std::string error)
    {
        draft.mPreviewedActive = true;
        draft.mPreviewedGeneration = generation;
        draft.mPreviewedParameters = std::string(overlayJson);
        draft.mPreviewError = std::move(error);
    }

    void MaterialDraftResetPreview(MaterialDraft &draft)
    {
        draft.mPreviewedFields.clear();
        draft.mPreviewedParameters.clear();
        draft.mPreviewedGeneration = 0;
        draft.mPreviewedActive = false;
        draft.mPreviewError.clear();
        draft.mPendingRestoreJson.clear();
    }

    bool MaterialFieldIsColor(std::string_view name)
    {
        return ContainsLower(name, "color") || ContainsLower(name, "colour") || ContainsLower(name, "tint") ||
            ContainsLower(name, "emissive");
    }

    std::vector<std::string> MaterialTextureRecipeRoles(std::string_view role)
    {
        if (role == "albedo")
            return {"baseColor", "ao"};
        if (role == "normal")
            return {"normal"};
        if (role == "surface")
            return {"roughness", "displacement", "specular"};
        return {};
    }
}
