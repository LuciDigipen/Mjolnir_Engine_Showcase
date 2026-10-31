/**
 * @file SurfaceMaterialLibrary.cpp
 * @author Jackey Cen
 * @brief Runtime ownership of cooked surface materials and their packed records.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/SurfaceMaterialLibrary.h>
#include <renderer/SurfaceMaterialTexture.h>

#include <assets/AssetCache.h>
#include <assets/ModelAsset.h>
#include <core/fs/FileIO.h>
#include <core/fs/Paths.h>
#include <renderer/MaterialReflection.h>
#include <renderer/SurfaceMaterialCatalog.h>
#include <renderer/VulkanRenderer.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <format>
#include <iterator>
#include <string>
#include <utility>

namespace mir
{
    namespace
    {
        using Json = nlohmann::json;

        void SetError(std::string &error, std::string message)
        {
            error = std::move(message);
        }

        std::string Lower(std::string_view text)
        {
            std::string result(text);
            std::transform(result.begin(), result.end(), result.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return result;
        }

        std::string MaterialKey(std::string_view logicalPath, std::string_view frontend)
        {
            return std::string(logicalPath) + "|" + std::string(frontend);
        }

        AssetId AssetForRole(const SurfaceMaterialAsset &asset, SurfaceTextureRole role)
        {
            switch (role)
            {
            case SurfaceTextureRole::Albedo:
                return asset.albedoTexture;
            case SurfaceTextureRole::Normal:
                return asset.normalTexture;
            case SurfaceTextureRole::Surface:
                return asset.surfaceTexture;
            case SurfaceTextureRole::Count:
                break;
            }
            return {};
        }
    }

    std::optional<SurfaceTextureRole> SurfaceTextureRoleForField(std::string_view field)
    {
        const std::string name = Lower(field);
        if (name == "albedo" || name == "albedotexture" || name == "basecolor" || name == "basecolortexture")
            return SurfaceTextureRole::Albedo;
        if (name == "normal" || name == "normaltexture")
            return SurfaceTextureRole::Normal;
        if (name == "surface" || name == "surfacetexture")
            return SurfaceTextureRole::Surface;
        return std::nullopt;
    }

    std::optional<double> SurfaceMaterialTilePeriodFromRecord(const MaterialLayout &layout,
        std::span<const std::byte> record)
    {
        for (const MaterialFieldLayout &field : layout.fields)
        {
            if (field.name != "tileSizeMeters" || field.kind != MaterialFieldKind::Float || field.size != sizeof(float) ||
                field.offset > record.size() || field.size > record.size() - field.offset)
                continue;
            float packed = 0.0f;
            std::memcpy(&packed, record.data() + field.offset, sizeof(packed));
            return static_cast<double>(packed);
        }
        return std::nullopt;
    }

    void SurfaceMaterialLibrary::TakeResources(std::vector<MaterialHandle> &materials,
        std::vector<TextureHandle> &textures) const
    {
        for (const auto &[key, entry] : mEntries)
            if (entry.binding)
                materials.push_back(entry.binding->material);
        for (const auto &[asset, texture] : mTextures)
            textures.push_back(texture);
        for (const TextureHandle texture : mDefaults)
            if (!texture.IsNull())
                textures.push_back(texture);
    }

    void SurfaceMaterialLibrary::ReleaseAll()
    {
        std::vector<MaterialHandle> materials;
        std::vector<TextureHandle> textures;
        TakeResources(materials, textures);
        if (mRenderer != nullptr)
            mRenderer->ReleaseModelResources(std::span<const MeshHandle>{}, materials, textures);
        mEntries.clear();
        mTextures.clear();
        mActive.clear();
        mActiveKeys.clear();
        for (TextureHandle &texture : mDefaults)
            texture = kNullTexture;
        ++mGeneration;
    }

    void SurfaceMaterialLibrary::SetMaterialValidator(SurfaceMaterialValidator validator)
    {
        mValidator = std::move(validator);
    }

    void SurfaceMaterialLibrary::SetActiveMaterials(std::span<const SurfaceMaterialUse> uses)
    {
        mActive.clear();
        mActiveKeys.clear();
        for (const SurfaceMaterialUse &use : uses)
        {
            const std::string key = MaterialKey(use.logicalPath, use.frontend);
            if (mActiveKeys.insert(key).second)
                mActive.push_back(use);
            else
                for (SurfaceMaterialUse &existing : mActive)
                    if (MaterialKey(existing.logicalPath, existing.frontend) == key)
                    {
                        existing.required = existing.required || use.required;
                        break;
                    }
        }
    }

    void SurfaceMaterialLibrary::Bind(AssetCache &assets, VulkanRenderer &renderer)
    {
        if (mRenderer != nullptr)
            ReleaseAll();
        mAssets = &assets;
        mRenderer = &renderer;
        ++mGeneration;
    }

    void SurfaceMaterialLibrary::Invalidate()
    {
        ReleaseAll();
    }

    void SurfaceMaterialLibrary::Reset()
    {
        ReleaseAll();
    }

    bool SurfaceMaterialLibrary::PrepareReload(AssetCache &candidateAssets, VulkanRenderer &renderer,
        std::string *error)
    {
        mCandidate.reset();
        if (mRenderer == nullptr)
        {
            if (error != nullptr)
                *error = "the material library is not bound";
            return false;
        }

        auto candidate = std::make_unique<SurfaceMaterialLibrary>();
        candidate->mAssets = &candidateAssets;
        candidate->mRenderer = &renderer;
        candidate->mGeneration = mGeneration + 1;
        candidate->mValidator = mValidator;

        for (const SurfaceMaterialUse &use : mActive)
        {
            const std::string key = MaterialKey(use.logicalPath, use.frontend);
            Entry staged;
            staged.logicalPath = use.logicalPath;
            staged.asset = use.asset;
            staged.frontend = use.frontend;
            SurfaceMaterialBinding binding;
            if (candidate->CreateFromAsset(use.logicalPath, use.asset, use.frontend, binding, staged.error).IsNull())
            {
                if (use.required)
                {
                    if (error != nullptr)
                        *error = std::format("material '{}': {}", use.logicalPath, staged.error);
                    candidate->ReleaseAll();
                    return false;
                }
                candidate->mEntries.emplace(key, std::move(staged));
                continue;
            }
            staged.binding = binding;
            candidate->mEntries.emplace(key, std::move(staged));
        }
        mCandidate = std::move(candidate);
        return true;
    }

    void SurfaceMaterialLibrary::CommitReload(AssetCache &assets)
    {
        if (!mCandidate)
            return;

        std::vector<MaterialHandle> oldMaterials;
        std::vector<TextureHandle> oldTextures;
        TakeResources(oldMaterials, oldTextures);

        mEntries = std::move(mCandidate->mEntries);
        mTextures = std::move(mCandidate->mTextures);
        for (size_t i = 0; i < static_cast<size_t>(SurfaceTextureRole::Count); ++i)
            mDefaults[i] = mCandidate->mDefaults[i];
        mAssets = &assets;
        mRenderer = mCandidate->mRenderer;
        mGeneration = mCandidate->mGeneration;
        mCandidate->mEntries.clear();
        mCandidate->mTextures.clear();
        for (TextureHandle &texture : mCandidate->mDefaults)
            texture = kNullTexture;
        mCandidate.reset();

        mRenderer->ReleaseModelResources({}, oldMaterials, oldTextures);
    }

    void SurfaceMaterialLibrary::AbortReload()
    {
        if (!mCandidate)
            return;
        mCandidate->ReleaseAll();
        mCandidate.reset();
    }

    TextureHandle SurfaceMaterialLibrary::DefaultTexture(SurfaceTextureRole role)
    {
        TextureHandle &cached = mDefaults[static_cast<size_t>(role)];
        if (!cached.IsNull() || mRenderer == nullptr)
            return cached;

        const std::array<std::byte, 4> white{
            std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
        const std::array<std::byte, 4> flatNormal{
            std::byte{128}, std::byte{128}, std::byte{255}, std::byte{255}};
        const std::array<std::byte, 4> flatSurface{
            std::byte{255}, std::byte{128}, std::byte{10}, std::byte{255}};

        switch (role)
        {
        case SurfaceTextureRole::Albedo:
            cached = mRenderer->CreateTexture(
                TextureDesc{.width = 1, .height = 1, .pixels = std::as_bytes(std::span(white)), .srgb = true});
            break;
        case SurfaceTextureRole::Normal:
            cached = mRenderer->CreateTexture(
                TextureDesc{.width = 1, .height = 1, .pixels = std::as_bytes(std::span(flatNormal)), .srgb = false});
            break;
        case SurfaceTextureRole::Surface:
            cached = mRenderer->CreateTexture(
                TextureDesc{.width = 1, .height = 1, .pixels = std::as_bytes(std::span(flatSurface)), .srgb = false});
            break;
        case SurfaceTextureRole::Count:
            break;
        }
        return cached;
    }

    TextureHandle SurfaceMaterialLibrary::EnsureTexture(SurfaceTextureRole role, AssetId asset, std::string &error)
    {
        if (asset.IsNull())
            return DefaultTexture(role);
        if (mAssets == nullptr || mRenderer == nullptr)
        {
            SetError(error, "the material library is not bound");
            return kNullTexture;
        }

        const auto cached = mTextures.find(asset.value);
        if (cached != mTextures.end())
            return cached->second;

        TextureAsset texture;
        if (!LoadTypedAsset(*mAssets, asset, texture, error))
            return kNullTexture;

        TextureDesc desc{.width = texture.width,
                         .height = texture.height,
                         .pixels = std::as_bytes(std::span(texture.rgba)),
                         .srgb = texture.format == TextureFormat::Rgba8Srgb,
                         .generateMips = texture.mips.empty()};
        for (const TextureMip &mip : texture.mips)
            desc.mips.push_back({mip.width, mip.height,
                std::as_bytes(std::span(texture.mipData).subspan(mip.offset, mip.length))});

        const TextureHandle handle = mRenderer->CreateTexture(desc);
        if (handle.IsNull())
        {
            SetError(error, "the packed texture could not be uploaded");
            return kNullTexture;
        }
        mTextures.emplace(asset.value, handle);
        return handle;
    }

    MaterialHandle SurfaceMaterialLibrary::CreateFromAsset(std::string_view logicalPath, AssetId asset,
        std::string_view frontend, SurfaceMaterialBinding &binding, std::string &error)
    {
        if (mAssets == nullptr || mRenderer == nullptr)
        {
            SetError(error, "the material library is not bound");
            return kNullMaterial;
        }

        SurfaceMaterialAsset materialAsset;
        if (!LoadTypedAsset(*mAssets, asset, materialAsset, error))
            return kNullMaterial;
        if (std::find(materialAsset.frontends.begin(), materialAsset.frontends.end(), frontend) ==
            materialAsset.frontends.end())
        {
            SetError(error, "the material does not declare the terrain frontend");
            return kNullMaterial;
        }

        const SurfaceMaterialRegistration *registration =
            mRenderer->SurfaceMaterials().FindByAsset(logicalPath, frontend);
        if (registration == nullptr)
        {
            SetError(error, "no cooked surface material registration names this asset and frontend");
            return kNullMaterial;
        }

        const std::filesystem::path reflectionPath = ShaderPath(registration->reflectionFile);
        const std::optional<std::string> reflection = ReadText(reflectionPath);
        if (!reflection)
        {
            SetError(error, "the material reflection is missing: " + reflectionPath.string());
            return kNullMaterial;
        }
        const std::optional<MaterialLayout> layout = ParseMaterialLayout(*reflection, &error);
        if (!layout)
            return kNullMaterial;

        Json parameters;
        try
        {
            parameters = Json::parse(materialAsset.parametersJson);
        }
        catch (const std::exception &exception)
        {
            SetError(error, std::string("the material parameters are not JSON: ") + exception.what());
            return kNullMaterial;
        }
        if (!parameters.is_object())
        {
            SetError(error, "the material parameters must be an object");
            return kNullMaterial;
        }

        const SamplerDesc sampler{.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                  .useMips = true,
                                  .maxAnisotropy = 4.0f};
        const uint32_t samplerRow = mRenderer->MaterialSamplerRow(sampler);
        if (samplerRow == UINT32_MAX)
        {
            SetError(error, "the material requires the shared sampler table");
            return kNullMaterial;
        }

        for (const MaterialFieldLayout &field : layout->fields)
        {
            if (field.kind != MaterialFieldKind::TextureRef)
                continue;
            const std::optional<SurfaceTextureRole> role = SurfaceTextureRoleForField(field.name);
            if (!role)
            {
                SetError(error, "the material texture field '" + field.name + "' has no packed role");
                return kNullMaterial;
            }
            const TextureHandle texture = EnsureTexture(*role, AssetForRole(materialAsset, *role), error);
            if (texture.IsNull())
                return kNullMaterial;
            if (field.size == 8)
                parameters[field.name] = Json{{"image", texture.mIndex}, {"sampler", samplerRow}};
            else
                parameters[field.name] = Json{{"image", texture.mIndex}};
        }

        const std::optional<std::vector<std::byte>> record = PackMaterialRecord(*layout, parameters.dump(), &error);
        if (!record)
            return kNullMaterial;

        binding.tilePeriodMeters = SurfaceMaterialTilePeriodFromRecord(*layout, *record);
        if (mValidator && !mValidator(binding, error))
            return kNullMaterial;

        MaterialDesc desc;
        desc.shaderName = registration->stem;
        desc.surfaceMaterial = true;
        desc.surfaceAsset = std::string(logicalPath);
        desc.sampler = sampler;
        desc.record = *record;
        binding.material = mRenderer->CreateMaterial(desc);
        if (binding.material.IsNull())
        {
            SetError(error, "the material pipeline or record could not be created");
            return kNullMaterial;
        }
        return binding.material;
    }

    std::optional<SurfaceMaterialBinding> SurfaceMaterialLibrary::Acquire(std::string_view logicalPath,
        AssetId asset, std::string_view frontend, std::string *error)
    {
        const std::string key = MaterialKey(logicalPath, frontend);
        const auto found = mEntries.find(key);
        if (found != mEntries.end())
        {
            if (!found->second.binding)
            {
                if (error != nullptr)
                    *error = found->second.error;
                return std::nullopt;
            }
            return found->second.binding;
        }

        Entry entry;
        entry.logicalPath = std::string(logicalPath);
        entry.asset = asset;
        entry.frontend = std::string(frontend);
        SurfaceMaterialBinding binding;
        if (!CreateFromAsset(logicalPath, asset, frontend, binding, entry.error).IsNull())
            entry.binding = binding;
        const auto [stored, inserted] = mEntries.emplace(key, std::move(entry));
        (void)inserted;
        if (!stored->second.binding)
        {
            if (error != nullptr)
                *error = stored->second.error;
            return std::nullopt;
        }
        return stored->second.binding;
    }
}
