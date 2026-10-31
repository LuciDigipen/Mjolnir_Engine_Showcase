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
#include <cmath>
#include <cstdint>
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
            case SurfaceTextureRole::Macro:
                return {};
            case SurfaceTextureRole::Count:
                break;
            }
            return {};
        }

        const MaterialFieldLayout *FindField(const MaterialLayout &layout, std::string_view name,
            MaterialFieldKind kind, uint32_t size)
        {
            for (const MaterialFieldLayout &field : layout.fields)
                if (field.name == name && field.kind == kind && field.size == size)
                    return &field;
            return nullptr;
        }

        template <typename T>
        bool ReadField(const MaterialLayout &layout, std::span<const std::byte> record, std::string_view name,
            MaterialFieldKind kind, T &out)
        {
            const MaterialFieldLayout *const field = FindField(layout, name, kind, sizeof(T));
            if (field == nullptr || field->offset > record.size() || field->size > record.size() - field->offset)
                return false;
            std::memcpy(&out, record.data() + field->offset, sizeof(T));
            return true;
        }

        bool HasField(const MaterialLayout &layout, std::string_view name)
        {
            for (const MaterialFieldLayout &field : layout.fields)
                if (field.name == name)
                    return true;
            return false;
        }

        bool ReadFloat(const MaterialLayout &layout, std::span<const std::byte> record, std::string_view name,
            double &out)
        {
            float packed = 0.0f;
            if (!ReadField(layout, record, name, MaterialFieldKind::Float, packed))
                return false;
            out = static_cast<double>(packed);
            return true;
        }

        void SetParameterError(std::string *error, std::string message)
        {
            if (error != nullptr)
                *error = std::move(message);
        }

        bool FiniteInRange(double value, double minimum, double maximum)
        {
            return std::isfinite(value) && value >= minimum && value <= maximum;
        }

        uint32_t HashLattice(int32_t x, int32_t y, uint32_t seed)
        {
            uint32_t hash = static_cast<uint32_t>(x) * 0x8da6b343u ^ static_cast<uint32_t>(y) * 0xd8163841u ^
                seed * 0xcb1ab31fu;
            hash ^= hash >> 16;
            hash *= 0x7feb352du;
            hash ^= hash >> 15;
            hash *= 0x846ca68bu;
            hash ^= hash >> 16;
            return hash;
        }

        float LatticeValue(int32_t x, int32_t y, uint32_t seed, int32_t period)
        {
            const int32_t wrappedX = ((x % period) + period) % period;
            const int32_t wrappedY = ((y % period) + period) % period;
            return static_cast<float>(HashLattice(wrappedX, wrappedY, seed) & 0xffffffu) / 16777215.0f;
        }

        float PeriodicValueNoise(float u, float v, int32_t period, uint32_t seed)
        {
            const float scaledU = u * static_cast<float>(period);
            const float scaledV = v * static_cast<float>(period);
            const int32_t x0 = static_cast<int32_t>(std::floor(scaledU));
            const int32_t y0 = static_cast<int32_t>(std::floor(scaledV));
            const float fx = scaledU - static_cast<float>(x0);
            const float fy = scaledV - static_cast<float>(y0);
            const float sx = fx * fx * (3.0f - 2.0f * fx);
            const float sy = fy * fy * (3.0f - 2.0f * fy);
            const float a = LatticeValue(x0, y0, seed, period);
            const float b = LatticeValue(x0 + 1, y0, seed, period);
            const float c = LatticeValue(x0, y0 + 1, seed, period);
            const float d = LatticeValue(x0 + 1, y0 + 1, seed, period);
            const float top = a + (b - a) * sx;
            const float bottom = c + (d - c) * sx;
            return top + (bottom - top) * sy;
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
        if (name == "macro" || name == "macrotexture")
            return SurfaceTextureRole::Macro;
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

    std::optional<SurfaceMaterialMacroParameters> SurfaceMaterialMacroParametersFromRecord(
        const MaterialLayout &layout, std::span<const std::byte> record)
    {
        if (!HasField(layout, "macroPeriodMeters") && !HasField(layout, "macroStrength"))
            return std::nullopt;

        SurfaceMaterialMacroParameters parameters;
        float period = 0.0f;
        if (ReadField(layout, record, "macroPeriodMeters", MaterialFieldKind::Float, period))
            parameters.periodMeters = static_cast<double>(period);
        ReadFloat(layout, record, "macroStrength", parameters.strength);
        ReadFloat(layout, record, "macroHueShiftDegrees", parameters.hueShiftDegrees);
        ReadFloat(layout, record, "macroRoughnessContrast", parameters.roughnessContrast);
        return parameters;
    }

    std::optional<SurfaceMaterialParallaxParameters> SurfaceMaterialParallaxParametersFromRecord(
        const MaterialLayout &layout, std::span<const std::byte> record)
    {
        if (!HasField(layout, "parallaxHeightMeters") && !HasField(layout, "parallaxEnabled"))
            return std::nullopt;

        SurfaceMaterialParallaxParameters parameters;
        uint32_t enabled = 0;
        if (ReadField(layout, record, "parallaxEnabled", MaterialFieldKind::Bool, enabled))
            parameters.enabled = enabled != 0;
        uint32_t rayTraced = 0;
        if (ReadField(layout, record, "parallaxRtEnabled", MaterialFieldKind::Bool, rayTraced))
            parameters.rayTracedEnabled = rayTraced != 0;
        ReadFloat(layout, record, "parallaxHeightMeters", parameters.heightMeters);
        ReadFloat(layout, record, "parallaxReference", parameters.reference);
        ReadFloat(layout, record, "parallaxDistanceMeters", parameters.maxDistanceMeters);
        ReadField(layout, record, "parallaxSteps", MaterialFieldKind::Uint, parameters.steps);
        ReadFloat(layout, record, "parallaxFadeStartMeters", parameters.fadeStartMeters);
        ReadFloat(layout, record, "parallaxFadeEndMeters", parameters.fadeEndMeters);
        return parameters;
    }

    bool ValidateSurfaceMaterialMacroParameters(const SurfaceMaterialMacroParameters &parameters, std::string *error)
    {
        if (parameters.periodMeters)
        {
            const double period = *parameters.periodMeters;
            if (!std::isfinite(period) || !(period > 0.0))
            {
                SetParameterError(error, "macroPeriodMeters must be a positive finite period");
                return false;
            }
        }
        if (!FiniteInRange(parameters.strength, 0.0, 1.0))
        {
            SetParameterError(error, "macroStrength must be within [0, 1]");
            return false;
        }
        if (!FiniteInRange(parameters.hueShiftDegrees, -180.0, 180.0))
        {
            SetParameterError(error, "macroHueShiftDegrees must be within [-180, 180]");
            return false;
        }
        if (!FiniteInRange(parameters.roughnessContrast, 0.0, 2.0))
        {
            SetParameterError(error, "macroRoughnessContrast must be within [0, 2]");
            return false;
        }
        return true;
    }

    bool ValidateSurfaceMaterialParallaxParameters(const SurfaceMaterialParallaxParameters &parameters,
        std::string *error)
    {
        if (!FiniteInRange(parameters.heightMeters, 0.0, 0.5))
        {
            SetParameterError(error, "parallaxHeightMeters must be within [0, 0.5]");
            return false;
        }
        if (!FiniteInRange(parameters.reference, 0.0, 1.0))
        {
            SetParameterError(error, "parallaxReference must be within [0, 1]");
            return false;
        }
        if (!FiniteInRange(parameters.maxDistanceMeters, 0.0, 0.5))
        {
            SetParameterError(error, "parallaxDistanceMeters must be within [0, 0.5]");
            return false;
        }
        if (parameters.steps > 128)
        {
            SetParameterError(error, "parallaxSteps must be at most 128");
            return false;
        }
        if (!FiniteInRange(parameters.fadeStartMeters, 0.0, 1.0e6) ||
            !FiniteInRange(parameters.fadeEndMeters, 0.0, 1.0e6) ||
            parameters.fadeEndMeters < parameters.fadeStartMeters)
        {
            SetParameterError(error, "parallax fade distances must be finite and ordered");
            return false;
        }
        if (parameters.enabled && !(parameters.heightMeters > 0.0))
        {
            SetParameterError(error, "parallaxHeightMeters must be positive while parallax is enabled");
            return false;
        }
        return true;
    }

    void BuildSurfaceMacroTexture(uint32_t size, std::vector<std::byte> &rgba)
    {
        const uint32_t resolution = std::max(size, 4u);
        rgba.assign(static_cast<size_t>(resolution) * resolution * 4, std::byte{255});
        constexpr int32_t kFinePeriod = 32;
        constexpr int32_t kCoarsePeriod = 8;
        for (uint32_t y = 0; y < resolution; ++y)
        {
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(resolution);
            for (uint32_t x = 0; x < resolution; ++x)
            {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(resolution);
                const float coarse = PeriodicValueNoise(u, v, kCoarsePeriod, 0x51ed270bu);
                const float fine = PeriodicValueNoise(u, v, kFinePeriod, 0x9e3779b9u);
                const float variation = std::clamp(0.72f * coarse + 0.28f * fine, 0.0f, 1.0f);
                const float hue = std::clamp(PeriodicValueNoise(u, v, kCoarsePeriod, 0x2545f491u), 0.0f, 1.0f);
                const float roughness = std::clamp(PeriodicValueNoise(u, v, kFinePeriod, 0x6a09e667u), 0.0f, 1.0f);
                auto &pixel = rgba[(static_cast<size_t>(y) * resolution + x) * 4];
                pixel = static_cast<std::byte>(variation * 255.0f + 0.5f);
                rgba[(static_cast<size_t>(y) * resolution + x) * 4 + 1] =
                    static_cast<std::byte>(hue * 255.0f + 0.5f);
                rgba[(static_cast<size_t>(y) * resolution + x) * 4 + 2] =
                    static_cast<std::byte>(roughness * 255.0f + 0.5f);
                rgba[(static_cast<size_t>(y) * resolution + x) * 4 + 3] = std::byte{255};
            }
        }
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
        case SurfaceTextureRole::Macro:
        {
            constexpr uint32_t kMacroSize = 256;
            std::vector<std::byte> pixels;
            BuildSurfaceMacroTexture(kMacroSize, pixels);
            cached = mRenderer->CreateTexture(TextureDesc{.width = kMacroSize,
                                                          .height = kMacroSize,
                                                          .pixels = std::as_bytes(std::span(pixels)),
                                                          .srgb = false,
                                                          .generateMips = true});
            break;
        }
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
        binding.macro = SurfaceMaterialMacroParametersFromRecord(*layout, *record);
        binding.parallax = SurfaceMaterialParallaxParametersFromRecord(*layout, *record);
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
