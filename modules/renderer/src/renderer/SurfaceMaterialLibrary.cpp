/**
 * @file SurfaceMaterialLibrary.cpp
 * @author Jackey Cen
 * @brief Runtime ownership of cooked surface materials and their packed records.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/SurfaceMaterialLibrary.h>
#include <renderer/SurfaceMaterialHost.h>
#include <renderer/SurfaceMaterialTexture.h>

#include <assets/AsyncAssetService.h>
#include <assets/ModelAsset.h>
#include <core/log/Log.h>
#include <renderer/ITextureUploader.h>
#include <renderer/MaterialReflection.h>
#include <renderer/SurfaceMaterialCatalog.h>
#include <renderer/TextureUpload.h>
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
#include <map>
#include <string>
#include <type_traits>
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

        std::span<const uint32_t> SurfaceMaterialIdentityTags()
        {
            static constexpr uint32_t kTags[]{ kSurfaceMaterialAssetTypeTag, kTextureAssetTypeTag };
            return kTags;
        }

        std::span<const uint32_t> TextureIdentityTags()
        {
            static constexpr uint32_t kTags[]{ kTextureAssetTypeTag };
            return kTags;
        }

        bool DecodeSurfaceMaterial(const AssetBlobView &root, const AssetDependencies &, SurfaceMaterialAsset &out,
            std::string &error)
        {
            return ReadTypedAsset(root, out, error);
        }

        uint64_t StringBytes(const std::string &value)
        {
            const char *object = reinterpret_cast<const char *>(&value);
            const bool inline_ = value.data() >= object && value.data() < object + sizeof(value);
            return inline_ ? 0 : value.capacity() + 1;
        }

        uint64_t SurfaceMaterialBytes(const SurfaceMaterialAsset &asset)
        {
            uint64_t bytes = sizeof(asset) + asset.frontends.capacity() * sizeof(std::string) +
                asset.textures.capacity() * sizeof(SurfaceMaterialTexture);
            bytes += StringBytes(asset.shaderFile) + StringBytes(asset.shaderType) +
                StringBytes(asset.parametersJson) + StringBytes(asset.unknownJson);
            for (const std::string &frontend : asset.frontends)
                bytes += StringBytes(frontend);
            for (const SurfaceMaterialTexture &texture : asset.textures)
                bytes += StringBytes(texture.role) + StringBytes(texture.path);
            return bytes;
        }

        const AssetDecoder<SurfaceMaterialAsset> kSurfaceMaterialDecoder{
            DecodeSurfaceMaterial, {}, SurfaceMaterialBytes };

        int FrontendRank(std::string_view frontend)
        {
            return frontend == "mesh" ? 0 : frontend == "terrain" ? 1 : 2;
        }

        void SetShadingValuesFromRecord(SurfaceMaterialBinding &binding, const MaterialLayout &layout,
            std::span<const std::byte> record)
        {
            binding.tilePeriodMeters = SurfaceMaterialTilePeriodFromRecord(layout, record);
            binding.macro = SurfaceMaterialMacroParametersFromRecord(layout, record);
            binding.parallax = SurfaceMaterialParallaxParametersFromRecord(layout, record);
        }
    }

    const AssetDecoder<SurfaceMaterialAsset> &SurfaceMaterialDecoder()
    {
        return kSurfaceMaterialDecoder;
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

    std::optional<SurfaceMaterialSchemaAdoption> AdoptSurfaceMaterialRecord(
        const SurfaceMaterialBinding &binding, const MaterialLayout &layout, std::span<const std::byte> record,
        std::string *error)
    {
        std::string localError;
        std::string &reported = error != nullptr ? *error : localError;
        const std::optional<std::string> parameters = MaterialRecordParameters(layout, record, &reported);
        if (!parameters)
            return std::nullopt;

        SurfaceMaterialSchemaAdoption adoption;
        adoption.binding = binding;
        adoption.layout = layout;
        adoption.parametersJson = *parameters;
        adoption.revision = MaterialLayoutRevision(layout);
        SetShadingValuesFromRecord(adoption.binding, layout, record);
        return adoption;
    }

    bool ValidateSurfaceMaterialParameterOverlay(std::span<const MaterialLayout> layouts,
        std::string_view parametersJson, std::string *error)
    {
        Json overlay;
        try
        {
            overlay = Json::parse(parametersJson);
        }
        catch (const Json::exception &exception)
        {
            SetParameterError(error, std::string("material parameters are not JSON: ") + exception.what());
            return false;
        }
        if (!overlay.is_object())
        {
            SetParameterError(error, "material parameters and overrides must be JSON objects");
            return false;
        }
        for (const auto &[name, value] : overlay.items())
        {
            bool known = false;
            for (const MaterialLayout &layout : layouts)
                for (const MaterialFieldLayout &field : layout.fields)
                {
                    if (field.name != name)
                        continue;
                    if (field.kind == MaterialFieldKind::TextureRef)
                    {
                        SetParameterError(error, "material texture '" + name + "' is edited through the source recipe");
                        return false;
                    }
                    known = true;
                }
            if (!known)
            {
                SetParameterError(error, "material parameter '" + name + "' is not in the reflected layout");
                return false;
            }
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

    SurfaceMaterialLibrary::SurfaceMaterialLibrary() = default;

    SurfaceMaterialLibrary::~SurfaceMaterialLibrary() = default;

    std::string SurfaceMaterialLibrary::MaterialKey(std::string_view logicalPath, std::string_view frontend)
    {
        return mir::MaterialKey(logicalPath, frontend);
    }

    void SurfaceMaterialLibrary::TakeResources(std::vector<MaterialHandle> &materials) const
    {
        for (const auto &[key, entry] : mEntries)
            if (entry.binding && !entry.binding->material.IsNull())
                materials.push_back(entry.binding->material);
        for (const RetiredResources &retired : mRetired)
            materials.insert(materials.end(), retired.materials.begin(), retired.materials.end());
        materials.insert(materials.end(), mUncommittedMaterials.begin(), mUncommittedMaterials.end());
    }

    void SurfaceMaterialLibrary::ReleaseAll()
    {
        std::vector<MaterialHandle> materials;
        TakeResources(materials);
        if (mHost != nullptr && !materials.empty())
            mHost->ReleaseSurfaceResources({}, materials, {});
        mRetired.clear();
        mUncommittedMaterials.clear();
        mEntries.clear();
        mLoads.clear();
        mBindingTextures.clear();
        mActive.clear();
        mActiveKeys.clear();
        mUsagePublished = false;
        mTerrainUses.clear();
        mTerrainAck = 0;
        for (auto &[owner, uses] : mUseOwners)
        {
            uses.clear();
            mOwnerAcks[owner] = mGeneration + 1;
        }
        ++mGeneration;
    }

    void SurfaceMaterialLibrary::Bind(AsyncAssetService *assets, VulkanRenderer &renderer)
    {
        if (mHost != nullptr)
            ReleaseAll();
        mOwnedHost = MakeVulkanSurfaceMaterialHost(renderer);
        mHost = mOwnedHost.get();
        mAssets = assets;
        mTextureLoads = renderer.TextureLoads(assets);
        ++mGeneration;
    }

    void SurfaceMaterialLibrary::Bind(AsyncAssetService *assets, SurfaceMaterialHost &host, ITextureUploader &textures)
    {
        if (mHost != nullptr)
            ReleaseAll();
        mOwnedHost.reset();
        mHost = &host;
        mAssets = assets;
        mTextureLoads = textures.TextureLoads(assets);
        ++mGeneration;
    }

    void SurfaceMaterialLibrary::Reset()
    {
        ReleaseAll();
    }

    void SurfaceMaterialLibrary::SetMaterialValidator(SurfaceMaterialValidator validator)
    {
        mValidator = std::move(validator);
    }

    SurfaceMaterialLookup SurfaceMaterialLibrary::LookupMeshSurface(std::string_view reference) const
    {
        SurfaceMaterialLookup lookup;
        if (mHost == nullptr)
        {
            lookup.status = SurfaceMaterialLookupStatus::Missing;
            lookup.error = "the material library is not bound";
            return lookup;
        }

        const bool explicitAsset = reference.ends_with(".material.json");
        std::size_t matches = 0;
        for (const SurfaceMaterialRegistration &registration : mHost->MaterialCatalog().materials)
        {
            if (registration.frontend != "mesh")
                continue;
            if (explicitAsset ? registration.asset == reference : registration.stem == reference)
            {
                if (matches == 0)
                    lookup.registration = &registration;
                ++matches;
            }
        }

        if (matches == 1)
        {
            lookup.status = SurfaceMaterialLookupStatus::Found;
            return lookup;
        }
        if (matches == 0)
        {
            if (explicitAsset)
            {
                lookup.status = SurfaceMaterialLookupStatus::Missing;
                lookup.error = "no mesh surface material is registered for asset '" + std::string(reference) + "'";
            }
            return lookup;
        }

        lookup.status = SurfaceMaterialLookupStatus::Ambiguous;
        lookup.registration = nullptr;
        lookup.error = "shader '" + std::string(reference) + "' names " + std::to_string(matches) +
            " surface materials; use its .material.json asset path";
        return lookup;
    }

    SurfaceMaterialPublication SurfaceMaterialLibrary::Publication(std::string_view logicalPath) const
    {
        SurfaceMaterialPublication publication;
        if (mAssets != nullptr)
            publication.revision = mAssets->Revision();

        std::vector<std::string> keys;
        for (const SurfaceMaterialUse &use : mActive)
            if (use.logicalPath == logicalPath)
                keys.push_back(MaterialKey(use.logicalPath, use.frontend));
        if (keys.empty())
        {
            for (const auto &[key, entry] : mEntries)
                if (entry.binding && entry.logicalPath == logicalPath)
                    keys.push_back(key);
        }
        if (keys.empty())
        {
            publication.status = SurfaceMaterialPublicationStatus::Inactive;
            return publication;
        }

        bool pending = false;
        uint64_t current = 0;
        for (const std::string &key : keys)
        {
            const auto found = mEntries.find(key);
            if (found == mEntries.end())
            {
                pending = true;
                continue;
            }
            const Entry &entry = found->second;
            if (current == 0)
                current = CurrentIdentity(entry.asset);
            if (const auto load = mLoads.find(key);
                load != mLoads.end() && load->second.failed)
            {
                publication.content = current;
                publication.status = SurfaceMaterialPublicationStatus::Failed;
                publication.error = load->second.error;
                return publication;
            }
            if (!entry.binding)
            {
                if (entry.failed)
                {
                    publication.content = current;
                    publication.status = SurfaceMaterialPublicationStatus::Failed;
                    publication.error = entry.error;
                    return publication;
                }
                pending = true;
                continue;
            }

            if (mLoads.contains(key))
            {
                pending = true;
                continue;
            }
            if (entry.attemptFailed)
            {
                if (entry.attemptContent == current)
                {
                    publication.content = current;
                    publication.status = SurfaceMaterialPublicationStatus::Failed;
                    publication.error = entry.attemptError;
                    return publication;
                }
                pending = true;
                continue;
            }
            if (entry.acceptedContent != current)
            {
                pending = true;
                continue;
            }
        }

        publication.content = current;
        publication.status = pending ? SurfaceMaterialPublicationStatus::Pending
                                     : SurfaceMaterialPublicationStatus::Published;
        return publication;
    }

    void SurfaceMaterialLibrary::NotifyShaderCommit() noexcept
    {
        ++mSchemaEpoch;
        ++mGeneration;
    }

    bool SurfaceMaterialLibrary::ValidateRecord(std::string_view frontend, const MaterialLayout &layout,
        std::span<const std::byte> bytes, std::string &error) const
    {
        SurfaceMaterialBinding binding;
        binding.tilePeriodMeters = SurfaceMaterialTilePeriodFromRecord(layout, bytes);
        binding.macro = SurfaceMaterialMacroParametersFromRecord(layout, bytes);
        binding.parallax = SurfaceMaterialParallaxParametersFromRecord(layout, bytes);

        // The shared numeric bounds hold for every frontend, so a mesh-only
        // material cannot publish a non-finite or out-of-range value either.
        if (binding.tilePeriodMeters)
        {
            const double period = *binding.tilePeriodMeters;
            if (!std::isfinite(period) || !(period > 0.0))
            {
                SetParameterError(&error, "tileSizeMeters must be a positive finite period");
                return false;
            }
        }
        if (binding.macro && !ValidateSurfaceMaterialMacroParameters(*binding.macro, &error))
            return false;
        if (binding.parallax && !ValidateSurfaceMaterialParallaxParameters(*binding.parallax, &error))
            return false;

        // The terrain consumer adds the UV-phase divisibility rule on top.
        if (frontend != "terrain" || !mValidator)
            return true;
        return mValidator(binding, error);
    }

    void SurfaceMaterialLibrary::SetRecordValidator(SurfaceMaterialRecordValidator validator)
    {
        mRecordValidator = std::move(validator);
    }

    bool SurfaceMaterialLibrary::ValidateRecordForPublication(std::string_view frontend,
        const MaterialLayout &layout, std::span<const std::byte> bytes, std::string &error) const
    {
        if (!ValidateRecord(frontend, layout, bytes, error))
            return false;
        return !mRecordValidator || mRecordValidator(layout, bytes, error);
    }

    void SurfaceMaterialLibrary::TryReleaseRetired()
    {
        if (mRetired.empty() && mUncommittedMaterials.empty())
            return;
        const auto acknowledged = [this](uint64_t generation)
        {
            for (const auto &[owner, ack] : mOwnerAcks)
                if (ack < generation)
                    return false;
            return mTerrainUses.empty() || mTerrainAck >= generation;
        };
        std::vector<MaterialHandle> materials;
        for (auto it = mRetired.begin(); it != mRetired.end();)
        {
            if (!acknowledged(it->generation))
            {
                ++it;
                continue;
            }
            materials.insert(materials.end(), it->materials.begin(), it->materials.end());
            it = mRetired.erase(it);
        }
        if (mHost != nullptr && !materials.empty())
            mHost->ReleaseSurfaceResources({}, materials, {});
        if (mHost != nullptr && !mUncommittedMaterials.empty())
        {
            mHost->ReleaseSurfaceResources({}, mUncommittedMaterials, {});
            mUncommittedMaterials.clear();
        }
    }

    void SurfaceMaterialLibrary::RebuildActiveMaterials()
    {
        // Terrain resolves bindings before it calls this, so a schema-replaced
        // handle is no longer the one any instance will draw next.
        mActive.clear();
        mActiveKeys.clear();
        const auto add = [this](const SurfaceMaterialUse &use)
        {
            const std::string key = MaterialKey(use.logicalPath, use.frontend);
            if (mActiveKeys.insert(key).second)
                mActive.push_back(use);
        };
        for (const SurfaceMaterialUse &use : mTerrainUses)
            add(use);
        for (const auto &[owner, uses] : mUseOwners)
            for (const SurfaceMaterialUse &use : uses)
                add(use);
        TryReleaseRetired();
    }

    void SurfaceMaterialLibrary::SetActiveMaterials(std::span<const SurfaceMaterialUse> uses)
    {
        mUsagePublished = true;
        mTerrainUses.assign(uses.begin(), uses.end());
        RebuildActiveMaterials();
    }

    uint64_t SurfaceMaterialLibrary::AddMaterialUseOwner()
    {
        const uint64_t owner = mNextUseOwner++;
        mUseOwners.emplace(owner, std::vector<SurfaceMaterialUse>{});
        mOwnerAcks[owner] = mGeneration;
        return owner;
    }

    void SurfaceMaterialLibrary::SetMaterialUses(uint64_t owner, std::span<const SurfaceMaterialUse> uses)
    {
        const auto found = mUseOwners.find(owner);
        if (found == mUseOwners.end())
            return;
        mUsagePublished = true;
        found->second.assign(uses.begin(), uses.end());
        RebuildActiveMaterials();
    }

    void SurfaceMaterialLibrary::RemoveMaterialUseOwner(uint64_t owner)
    {
        if (mUseOwners.erase(owner) == 0)
            return;
        mOwnerAcks.erase(owner);
        RebuildActiveMaterials();
    }

    void SurfaceMaterialLibrary::AcknowledgeGeneration(uint64_t owner, uint64_t generation)
    {
        if (owner == 0)
            mTerrainAck = std::max(mTerrainAck, generation);
        else if (const auto found = mOwnerAcks.find(owner); found != mOwnerAcks.end())
            found->second = std::max(found->second, generation);
        TryReleaseRetired();
    }

    uint64_t SurfaceMaterialLibrary::CurrentIdentity(AssetId asset) const
    {
        if (mAssets == nullptr)
            return 0;
        const uint64_t content = mAssets->ContentOfTags(asset, SurfaceMaterialIdentityTags());
        return content != 0 ? content : 0x8000000000000000ull | (uint64_t(mAssets->Revision()) + 1);
    }

    SurfaceMaterialLibrary::Entry &SurfaceMaterialLibrary::EnsureEntry(std::string_view logicalPath, AssetId asset,
        std::string_view frontend)
    {
        const std::string key = MaterialKey(logicalPath, frontend);
        const auto [stored, inserted] = mEntries.try_emplace(key);
        Entry &entry = stored->second;
        if (inserted)
        {
            entry.logicalPath = std::string(logicalPath);
            entry.asset = asset;
            entry.frontend = std::string(frontend);
        }
        return entry;
    }

    bool SurfaceMaterialLibrary::StartLoad(Entry &entry)
    {
        if (mAssets == nullptr || mHost == nullptr)
        {
            entry.failed = true;
            entry.error = "the material library is not bound to an asset service";
            entry.attemptRevision = 0;
            return false;
        }
        const std::string key = MaterialKey(entry.logicalPath, entry.frontend);
        mLoads.erase(key);
        entry.failed = false;
        entry.error.clear();
        entry.attemptFailed = false;
        entry.attemptError.clear();
        entry.attemptContent = 0;

        Load load;
        load.identity = CurrentIdentity(entry.asset);
        load.revision = mAssets->Revision();
        load.assetLease = mAssets->RequestLease(entry.asset, SurfaceMaterialDecoder());
        mLoads.emplace(key, std::move(load));
        return true;
    }

    bool SurfaceMaterialLibrary::ParseLoad(Entry &entry, Load &load, std::string &error)
    {
        const auto asset = load.assetLease.Acquire();
        if (!asset)
        {
            error = "its decoded data is missing";
            return false;
        }
        if (std::find(asset->frontends.begin(), asset->frontends.end(), entry.frontend) == asset->frontends.end())
        {
            error = "the material does not declare the requested frontend";
            return false;
        }

        const SurfaceMaterialRegistration *registration =
            mHost->MaterialCatalog().FindByAsset(entry.logicalPath, entry.frontend);
        if (registration == nullptr)
        {
            error = "no cooked surface material registration names this asset and frontend";
            return false;
        }
        const std::optional<SurfaceMaterialSchema> schema = mHost->LoadSchema(*registration, &error);
        if (!schema)
            return false;

        Json parameters;
        try
        {
            parameters = Json::parse(asset->parametersJson);
        }
        catch (const std::exception &exception)
        {
            error = std::string("the material parameters are not JSON: ") + exception.what();
            return false;
        }
        if (!parameters.is_object())
        {
            error = "the material parameters must be an object";
            return false;
        }

        if (mTextureLoads == nullptr)
        {
            error = "the texture uploader is not bound";
            return false;
        }
        for (const MaterialFieldLayout &field : schema->layout.fields)
        {
            if (field.kind != MaterialFieldKind::TextureRef)
                continue;
            const std::optional<SurfaceTextureRole> role = SurfaceTextureRoleForField(field.name);
            if (!role)
            {
                error = "the material texture field '" + field.name + "' has no packed role";
                return false;
            }
            if (!EnsureTextureLoad(entry, load, *role, *asset, error))
                return false;
        }

        load.materialAsset = asset;
        load.parsed = true;
        load.assetLease.Reset();
        return true;
    }

    bool SurfaceMaterialLibrary::EnsureTextureLoad(Entry &entry, Load &load, SurfaceTextureRole role,
        const SurfaceMaterialAsset &material, std::string &error)
    {
        const uint32_t index = static_cast<uint32_t>(role);
        if (load.textures.contains(index))
            return true;
        if (mTextureLoads == nullptr)
        {
            error = "the texture uploader is not bound";
            return false;
        }

        const AssetId textureAsset = AssetForRole(material, role);
        if (!textureAsset.IsNull())
        {
            const UploadKey key{ textureAsset, UploadContent(mAssets, textureAsset, TextureIdentityTags()) };
            mTextureLoads->Request(key, std::format("{} texture {:016x}", entry.logicalPath, textureAsset.value));
            load.textures.emplace(index, TextureLoadLease(mTextureLoads, key));
            return true;
        }

        const AssetId defaultAsset{ 0x4d52534400000000ull + index };
        const UploadKey key{ defaultAsset, 1 };
        mTextureLoads->RequestInline(key,
            [role]() -> std::shared_ptr<const TextureUploadSource>
            {
                TextureDesc desc;
                switch (role)
                {
                case SurfaceTextureRole::Albedo:
                {
                    static constexpr std::array<std::byte, 4> kWhite{
                        std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255} };
                    desc = TextureDesc{ .width = 1, .height = 1, .pixels = std::as_bytes(std::span(kWhite)),
                        .srgb = true };
                    break;
                }
                case SurfaceTextureRole::Normal:
                {
                    static constexpr std::array<std::byte, 4> kFlatNormal{
                        std::byte{128}, std::byte{128}, std::byte{255}, std::byte{255} };
                    desc = TextureDesc{ .width = 1, .height = 1, .pixels = std::as_bytes(std::span(kFlatNormal)),
                        .srgb = false };
                    break;
                }
                case SurfaceTextureRole::Surface:
                {
                    static constexpr std::array<std::byte, 4> kFlatSurface{
                        std::byte{255}, std::byte{128}, std::byte{10}, std::byte{255} };
                    desc = TextureDesc{ .width = 1, .height = 1, .pixels = std::as_bytes(std::span(kFlatSurface)),
                        .srgb = false };
                    break;
                }
                case SurfaceTextureRole::Macro:
                {
                    constexpr uint32_t kMacroSize = 256;
                    std::vector<std::byte> pixels;
                    BuildSurfaceMacroTexture(kMacroSize, pixels);
                    TextureDesc macro{ .width = kMacroSize, .height = kMacroSize,
                        .pixels = std::as_bytes(std::span(pixels)), .srgb = false, .generateMips = true };
                    TextureUploadSource source = PrepareTextureUpload(macro);
                    if (source.desc.width == 0)
                        return nullptr;
                    return std::make_shared<const TextureUploadSource>(std::move(source));
                }
                case SurfaceTextureRole::Count:
                    return nullptr;
                }
                TextureUploadSource source = PrepareTextureUpload(desc);
                if (source.desc.width == 0)
                    return nullptr;
                return std::make_shared<const TextureUploadSource>(std::move(source));
            },
            std::format("{} default texture {}", entry.logicalPath, index));
        load.textures.emplace(index, TextureLoadLease(mTextureLoads, key));
        return true;
    }

    bool SurfaceMaterialLibrary::TexturesSettled(const Load &load) const
    {
        for (const auto &[role, claim] : load.textures)
        {
            const TextureLoad *view = claim.Get();
            if (view == nullptr || (view->phase != AssetUploadPhase::Ready && view->phase != AssetUploadPhase::Failed))
                return false;
        }
        return true;
    }

    bool SurfaceMaterialLibrary::BuildCandidateData(Entry &entry, Load &load, const MaterialLayout *peerLayout,
        std::span<const std::byte> peerRecord, std::string &error)
    {
        const SurfaceMaterialRegistration *registration =
            mHost->MaterialCatalog().FindByAsset(entry.logicalPath, entry.frontend);
        if (registration == nullptr)
        {
            error = "no cooked surface material registration names this asset and frontend";
            return false;
        }
        const std::optional<SurfaceMaterialSchema> schema = mHost->LoadSchema(*registration, &error);
        if (!schema)
            return false;
        const MaterialLayout &layout = schema->layout;

        Json parameters;
        try
        {
            parameters = Json::parse(load.materialAsset->parametersJson);
        }
        catch (const std::exception &exception)
        {
            error = std::string("the material parameters are not JSON: ") + exception.what();
            return false;
        }
        if (!parameters.is_object())
        {
            error = "the material parameters must be an object";
            return false;
        }

        const SamplerDesc sampler{ .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                   .useMips = true,
                                   .maxAnisotropy = 4.0f };
        const uint32_t samplerRow = mHost->MaterialSamplerRow(sampler);
        if (samplerRow == UINT32_MAX)
        {
            error = "the material requires the shared sampler table";
            return false;
        }

        for (const MaterialFieldLayout &field : layout.fields)
        {
            if (field.kind != MaterialFieldKind::TextureRef)
                continue;
            const std::optional<SurfaceTextureRole> role = SurfaceTextureRoleForField(field.name);
            if (!role)
            {
                error = "the material texture field '" + field.name + "' has no packed role";
                return false;
            }
            const auto found = load.textures.find(static_cast<uint32_t>(*role));
            if (found == load.textures.end())
            {
                error = "the packed texture was not requested";
                return false;
            }
            const TextureLoad *view = found->second.Get();
            if (view == nullptr || view->phase != AssetUploadPhase::Ready)
            {
                error = "the packed texture did not load";
                return false;
            }
            const TextureHandle texture = view->texture;
            if (texture.IsNull())
            {
                error = "the material texture could not be uploaded";
                return false;
            }
            if (field.size == 8)
                parameters[field.name] = Json{ { "image", texture.mIndex }, { "sampler", samplerRow } };
            else
                parameters[field.name] = Json{ { "image", texture.mIndex } };
        }

        if (peerLayout != nullptr && !peerRecord.empty())
        {
            // A later frontend joins the numeric values already live on its peer;
            // its own TextureRef rows stay those the cooker resolved.
            const std::optional<std::string> merged =
                MergePeerNumericParameters(layout, parameters.dump(), *peerLayout, peerRecord, &error);
            if (!merged)
                return false;
            parameters = Json::parse(*merged);
        }

        const std::optional<std::string> complete =
            CompleteMaterialParameters(layout, parameters.dump(), registration->parametersJson, &error);
        if (!complete)
            return false;
        const std::optional<std::vector<std::byte>> record = PackMaterialRecord(layout, *complete, &error);
        if (!record)
            return false;

        SurfaceMaterialBinding binding;
        SetShadingValuesFromRecord(binding, layout, *record);
        if (!ValidateRecordForPublication(entry.frontend, layout, *record, error))
            return false;

        load.candidate = binding;
        load.candidateLayout = layout;
        load.candidateParametersJson = *complete;
        load.candidateRevision = schema->revision;
        load.candidateRecord = *record;
        load.candidateBuilt = true;
        return true;
    }

    void SurfaceMaterialLibrary::FailLoad(Entry &entry, Load &load, std::string error)
    {
        const bool hadBinding = entry.binding.has_value();
        if (hadBinding)
            MIR_LOG_WARN("Surface material '{}' frontend '{}': replacement rejected, keeping the previous material: {}",
                entry.logicalPath, entry.frontend, error);
        else
        {
            entry.error = error;
            entry.failed = true;
            entry.acceptedContent = 0;
        }
        entry.attemptRevision = load.revision;
        entry.attemptFailed = true;
        entry.attemptError = error;
        entry.attemptContent = load.identity;
        mLoads.erase(MaterialKey(entry.logicalPath, entry.frontend));
        ++mGeneration;
    }

    void SurfaceMaterialLibrary::PublishLoad(Entry &entry, Load &load, uint64_t generation)
    {
        const std::string key = MaterialKey(entry.logicalPath, entry.frontend);
        RetiredResources retired;
        retired.generation = generation;
        if (entry.binding && !entry.binding->material.IsNull())
            retired.materials.push_back(entry.binding->material);

        const auto oldTextures = mBindingTextures.find(key);
        if (oldTextures != mBindingTextures.end())
        {
            for (TextureLoadLease &claim : oldTextures->second)
                retired.textures.push_back(std::move(claim));
            mBindingTextures.erase(oldTextures);
        }
        if (!retired.materials.empty() || !retired.textures.empty())
            mRetired.push_back(std::move(retired));

        entry.binding = load.candidate;
        entry.layout = load.candidateLayout;
        entry.parametersJson = load.candidateParametersJson;
        entry.schemaRevision = load.candidateRevision;
        entry.mSchemaEpoch = mSchemaEpoch;
        entry.failed = false;
        entry.error.clear();
        entry.attemptFailed = false;
        entry.attemptError.clear();
        entry.attemptContent = 0;
        entry.acceptedContent = load.identity;
        entry.acceptedRevision = load.revision;
        std::vector<TextureLoadLease> claims;
        claims.reserve(load.textures.size());
        for (auto &[role, claim] : load.textures)
            claims.push_back(std::move(claim));
        mBindingTextures.emplace(key, std::move(claims));
        mLoads.erase(key);
    }

    void SurfaceMaterialLibrary::PruneUnused()
    {
        if (!mUsagePublished)
            return;
        std::unordered_set<std::string> live;
        for (const SurfaceMaterialUse &use : mActive)
            live.insert(MaterialKey(use.logicalPath, use.frontend));

        for (auto it = mEntries.begin(); it != mEntries.end();)
        {
            if (live.contains(it->first))
            {
                ++it;
                continue;
            }
            Entry &entry = it->second;
            RetiredResources retired;
            retired.generation = mGeneration;
            if (entry.binding && !entry.binding->material.IsNull())
                retired.materials.push_back(entry.binding->material);
            if (const auto textures = mBindingTextures.find(it->first); textures != mBindingTextures.end())
            {
                for (TextureLoadLease &claim : textures->second)
                    retired.textures.push_back(std::move(claim));
                mBindingTextures.erase(textures);
            }
            if (!retired.materials.empty() || !retired.textures.empty())
                mRetired.push_back(std::move(retired));
            mLoads.erase(it->first);
            it = mEntries.erase(it);
        }
    }

    void SurfaceMaterialLibrary::Advance()
    {
        if (mTextureLoads != nullptr)
            mTextureLoads->Update();
        if (mHost == nullptr)
        {
            TryReleaseRetired();
            return;
        }

        PruneUnused();

        std::map<uint64_t, std::vector<std::string>> groups;
        for (auto &[key, load] : mLoads)
        {
            const auto entry = mEntries.find(key);
            if (entry != mEntries.end())
                groups[entry->second.asset.value].push_back(key);
        }

        for (auto &[asset, keys] : groups)
        {
            (void)asset;
            bool pending = false;
            std::string failure;
            for (const std::string &key : keys)
            {
                Entry &entry = mEntries.at(key);
                Load &load = mLoads.at(key);
                if (load.failed)
                {
                    if (failure.empty())
                        failure = load.error;
                    continue;
                }
                if (!load.parsed)
                {
                    const AssetLoadState state = load.assetLease.State();
                    if (state == AssetLoadState::Queued)
                    {
                        pending = true;
                        continue;
                    }
                    if (state != AssetLoadState::Ready)
                    {
                        load.failed = true;
                        load.error = state == AssetLoadState::Failed ? load.assetLease.FailureReason()
                                                                     : "its decoded data is no longer available";
                        failure = load.error;
                        continue;
                    }
                    std::string error;
                    if (!ParseLoad(entry, load, error))
                    {
                        load.failed = true;
                        load.error = std::move(error);
                        failure = load.error;
                        continue;
                    }
                }
                if (load.failed)
                {
                    if (failure.empty())
                        failure = load.error;
                    continue;
                }
                if (!TexturesSettled(load))
                {
                    pending = true;
                    continue;
                }
                for (const auto &[role, claim] : load.textures)
                {
                    const TextureLoad *view = claim.Get();
                    if (view == nullptr || view->phase == AssetUploadPhase::Failed)
                    {
                        load.failed = true;
                        load.error = "the packed texture did not load";
                        failure = load.error;
                        break;
                    }
                }
            }
            if (!failure.empty())
            {
                // One asset is one replacement transaction. No frontend of it may publish when another one
                // fails, so every consumer keeps the old generation it already holds.
                for (const std::string &key : keys)
                {
                    const auto found = mLoads.find(key);
                    if (found == mLoads.end())
                        continue;
                    FailLoad(mEntries.at(key), found->second, failure);
                }
                continue;
            }
            if (pending)
                continue;

            std::vector<std::string> participants = keys;
            std::sort(participants.begin(), participants.end(),
                [&](const std::string &left, const std::string &right)
                {
                    return FrontendRank(mEntries.at(left).frontend) < FrontendRank(mEntries.at(right).frontend);
                });

            for (const std::string &key : participants)
            {
                Load &load = mLoads.at(key);
                if (load.candidateBuilt)
                    continue;
                Entry &entry = mEntries.at(key);
                const MaterialLayout *peerLayout = nullptr;
                std::optional<MaterialRecordSnapshot> peerRecord;
                for (const std::string &otherKey : participants)
                {
                    if (otherKey == key)
                        continue;
                    Load &otherLoad = mLoads.at(otherKey);
                    if (otherLoad.candidateBuilt && !otherLoad.failed)
                    {
                        peerLayout = &otherLoad.candidateLayout;
                        peerRecord.emplace();
                        peerRecord->mLayout = otherLoad.candidateLayout;
                        peerRecord->mBytes = otherLoad.candidateRecord;
                        break;
                    }
                }
                if (peerLayout == nullptr && participants.size() == 1)
                {
                    // A single frontend joining an asset whose peer is already live keeps the peer's numeric
                    // values. A batch of frontends is a full replacement and starts from its new cooked values.
                    for (auto &[otherKey, otherEntry] : mEntries)
                    {
                        if (otherKey == key || !otherEntry.binding || otherEntry.asset != entry.asset)
                            continue;
                        const auto snapshot = mHost->MaterialRecord(otherEntry.binding->material);
                        if (snapshot)
                        {
                            peerRecord = snapshot;
                            peerLayout = &peerRecord->mLayout;
                        }
                        break;
                    }
                }

                std::string error;
                const std::span<const std::byte> peerBytes =
                    peerRecord ? std::span<const std::byte>(peerRecord->mBytes) : std::span<const std::byte>{};
                if (!BuildCandidateData(entry, load, peerLayout, peerBytes, error))
                {
                    load.failed = true;
                    load.error = std::move(error);
                    failure = load.error;
                }
            }
            if (failure.empty())
            {
                std::vector<MaterialHandle> created;
                for (const std::string &key : participants)
                {
                    Load &load = mLoads.at(key);
                    if (load.candidateCreated)
                        continue;
                    Entry &entry = mEntries.at(key);
                    const SurfaceMaterialRegistration *registration =
                        mHost->MaterialCatalog().FindByAsset(entry.logicalPath, entry.frontend);
                    if (registration == nullptr)
                    {
                        load.failed = true;
                        load.error = "no cooked surface material registration names this asset and frontend";
                        failure = load.error;
                        break;
                    }
                    const SamplerDesc sampler{ .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                               .useMips = true,
                                               .maxAnisotropy = 4.0f };
                    MaterialDesc desc;
                    desc.shaderName = registration->stem;
                    desc.surfaceMaterial = true;
                    desc.surfaceAsset = entry.logicalPath;
                    desc.sampler = sampler;
                    desc.record = load.candidateRecord;
                    const MaterialHandle material = mHost->CreateSurfaceMaterial(desc);
                    if (material.IsNull())
                    {
                        load.failed = true;
                        load.error = "the material pipeline or record could not be created";
                        failure = load.error;
                        break;
                    }
                    load.candidate.material = material;
                    load.candidateCreated = true;
                    created.push_back(material);
                }
                if (!failure.empty())
                    mUncommittedMaterials.insert(mUncommittedMaterials.end(), created.begin(), created.end());
            }

            if (!failure.empty())
            {
                for (const std::string &key : keys)
                {
                    const auto found = mLoads.find(key);
                    if (found == mLoads.end())
                        continue;
                    FailLoad(mEntries.at(key), found->second, failure);
                }
                continue;
            }

            const uint64_t generation = mGeneration + 1;
            mGeneration = generation;
            for (const std::string &key : keys)
            {
                const auto found = mLoads.find(key);
                if (found == mLoads.end())
                    continue;
                PublishLoad(mEntries.at(key), found->second, generation);
            }
        }
        TryReleaseRetired();
    }

    void SurfaceMaterialLibrary::CommitReload()
    {
        if (mAssets == nullptr || mHost == nullptr)
            return;

        PruneUnused();
        std::vector<std::string> keys;
        keys.reserve(mActive.size());
        for (const SurfaceMaterialUse &use : mActive)
        {
            Entry &entry = EnsureEntry(use.logicalPath, use.asset, use.frontend);
            keys.push_back(MaterialKey(entry.logicalPath, entry.frontend));
        }

        for (const std::string &key : keys)
        {
            const auto found = mEntries.find(key);
            if (found == mEntries.end())
                continue;
            Entry &entry = found->second;
            const uint64_t identity = CurrentIdentity(entry.asset);
            if (entry.binding && identity == entry.acceptedContent)
            {
                mLoads.erase(key);
                entry.acceptedRevision = mAssets->Revision();
                entry.failed = false;
                entry.error.clear();
                entry.attemptFailed = false;
                entry.attemptError.clear();
                entry.attemptContent = 0;
                continue;
            }
            const auto load = mLoads.find(key);
            if (load != mLoads.end() && !load->second.failed && load->second.identity == identity)
                continue;
            StartLoad(entry);
        }
        ++mGeneration;
    }

    SurfaceMaterialResolution SurfaceMaterialLibrary::Resolve(std::string_view logicalPath, AssetId asset,
        std::string_view frontend)
    {
        SurfaceMaterialResolution resolution;
        if (mHost == nullptr)
        {
            resolution.error = "the material library is not bound";
            return resolution;
        }
        Entry &entry = EnsureEntry(logicalPath, asset, frontend);
        const std::string key = MaterialKey(logicalPath, frontend);

        if (entry.binding)
        {
            if (entry.mSchemaEpoch != mSchemaEpoch)
            {
                std::string error;
                if (!AdoptAcceptedRecord(entry, error))
                {
                    resolution.error = std::move(error);
                    return resolution;
                }
            }
            if (mAssets != nullptr)
            {
                const uint32_t revision = mAssets->Revision();
                if (entry.acceptedRevision != revision && entry.attemptRevision != revision)
                {
                    const uint64_t identity = CurrentIdentity(entry.asset);
                    if (identity == entry.acceptedContent)
                        entry.acceptedRevision = revision;
                    else
                    {
                        const auto load = mLoads.find(key);
                        if (load == mLoads.end())
                            StartLoad(entry);
                    }
                }
            }
            resolution.status = SurfaceMaterialStatus::Ready;
            resolution.binding = entry.binding;
            return resolution;
        }

        if (mAssets == nullptr)
        {
            entry.failed = true;
            entry.error = "there is no asset service";
            resolution.status = SurfaceMaterialStatus::Failed;
            resolution.error = entry.error;
            return resolution;
        }

        const uint32_t revision = mAssets->Revision();
        if (entry.failed && entry.attemptRevision == revision)
        {
            resolution.status = SurfaceMaterialStatus::Failed;
            resolution.error = entry.error;
            return resolution;
        }

        const auto load = mLoads.find(key);
        if (load == mLoads.end() || load->second.failed)
        {
            if (load != mLoads.end())
                mLoads.erase(load);
            if (!StartLoad(entry))
            {
                resolution.status = SurfaceMaterialStatus::Failed;
                resolution.error = entry.error;
                return resolution;
            }
        }
        resolution.status = SurfaceMaterialStatus::Pending;
        return resolution;
    }

    bool SurfaceMaterialLibrary::IsActive(std::string_view logicalPath, std::string_view frontend) const
    {
        const auto found = mEntries.find(MaterialKey(logicalPath, frontend));
        return found != mEntries.end() && found->second.binding.has_value();
    }

    bool SurfaceMaterialLibrary::IsActiveAsset(std::string_view logicalPath) const
    {
        for (const auto &[key, entry] : mEntries)
            if (entry.binding && entry.logicalPath == logicalPath)
                return true;
        return false;
    }

    std::optional<SurfaceMaterialSchema> SurfaceMaterialLibrary::CurrentSchema(std::string_view logicalPath,
        std::string_view frontend, std::string *error) const
    {
        std::string localError;
        std::string &reported = error != nullptr ? *error : localError;
        if (mHost == nullptr)
        {
            SetError(reported, "the material library is not bound");
            return std::nullopt;
        }
        const SurfaceMaterialRegistration *registration =
            mHost->MaterialCatalog().FindByAsset(logicalPath, frontend);
        if (registration == nullptr)
        {
            SetError(reported, "no cooked surface material registration names this asset and frontend");
            return std::nullopt;
        }
        return mHost->LoadSchema(*registration, error);
    }

    bool SurfaceMaterialLibrary::AdoptAcceptedRecord(Entry &entry, std::string &error)
    {
        if (mHost == nullptr || !entry.binding)
        {
            SetError(error, "the material library is not bound");
            return false;
        }
        const std::optional<MaterialRecordSnapshot> snapshot = mHost->MaterialRecord(entry.binding->material);
        if (!snapshot)
        {
            SetError(error, "the committed material record is unavailable");
            return false;
        }
        std::string adoptionError;
        const std::optional<SurfaceMaterialSchemaAdoption> adopted =
            AdoptSurfaceMaterialRecord(*entry.binding, snapshot->mLayout, snapshot->mBytes, &adoptionError);
        if (!adopted)
        {
            SetError(error, adoptionError);
            return false;
        }
        if (!ValidateRecordForPublication(entry.frontend, adopted->layout, snapshot->mBytes, error))
            return false;

        entry.binding = adopted->binding;
        entry.layout = adopted->layout;
        entry.parametersJson = adopted->parametersJson;
        entry.schemaRevision = adopted->revision;
        entry.mSchemaEpoch = mSchemaEpoch;
        return true;
    }

    bool SurfaceMaterialLibrary::PrepareSchemaCandidate(const Entry &entry, Entry &candidate, bool &changed,
        std::string &error)
    {
        changed = false;
        if (mHost == nullptr)
        {
            SetError(error, "the material library is not bound");
            return false;
        }
        const SurfaceMaterialRegistration *registration =
            mHost->MaterialCatalog().FindByAsset(entry.logicalPath, entry.frontend);
        if (registration == nullptr)
        {
            SetError(error, "no cooked surface material registration names this asset and frontend");
            return false;
        }
        const std::optional<SurfaceMaterialSchema> schema = mHost->LoadSchema(*registration, &error);
        if (!schema)
            return false;
        if (schema->revision == entry.schemaRevision && entry.mSchemaEpoch == mSchemaEpoch)
            return true;

        Entry adopted = entry;
        if (!AdoptAcceptedRecord(adopted, error))
            return false;
        if (adopted.layout.fields.empty() || adopted.schemaRevision != schema->revision)
        {
            SetError(error, "the compiled material schema has no committed shader generation yet; "
                            "the preview waits for the shader reload");
            return false;
        }
        candidate = std::move(adopted);
        changed = true;
        return true;
    }

    bool SurfaceMaterialLibrary::PreviewParameters(std::string_view logicalPath, std::string_view,
        std::string_view parametersJson, std::string *error)
    {
        std::string localError;
        std::string &reported = error != nullptr ? *error : localError;

        if (mHost == nullptr)
        {
            SetError(reported, "the material library is not bound");
            return false;
        }
        const std::string assetPath(logicalPath);

        struct StagedTarget
        {
            std::string mKey;
            Entry mEntry;
            std::string mResolvedJson;
            std::vector<std::byte> mBytes;
            SurfaceMaterialBinding mBinding;
            bool mChanged = false;
            bool mSchemaChanged = false;
        };

        // Every valid frontend of one asset previews together. Nothing here
        // touches a cached entry: a later rejection must leave every frontend on
        // the bytes, metadata and generation it had.
        std::vector<StagedTarget> staged;
        std::vector<MaterialLayout> layouts;
        for (auto &[key, sibling] : mEntries)
        {
            if (!sibling.binding || sibling.logicalPath != assetPath)
                continue;

            const std::optional<MaterialRecordSnapshot> snapshot =
                mHost->MaterialRecord(sibling.binding->material);
            if (!snapshot)
            {
                SetError(reported, "the committed material record is unavailable");
                return false;
            }
            const std::optional<std::string> current =
                MaterialRecordParameters(snapshot->mLayout, snapshot->mBytes, &reported);
            if (!current)
                return false;

            StagedTarget target;
            target.mKey = key;
            target.mEntry = sibling;
            target.mEntry.layout = snapshot->mLayout;
            target.mEntry.parametersJson = *current;

            Entry adopted;
            bool schemaChanged = false;
            if (!PrepareSchemaCandidate(target.mEntry, adopted, schemaChanged, reported))
                return false;
            if (schemaChanged)
                target.mEntry = std::move(adopted);
            target.mSchemaChanged = schemaChanged;

            target.mBytes = snapshot->mBytes;
            layouts.push_back(target.mEntry.layout);
            staged.push_back(std::move(target));
        }
        if (staged.empty())
        {
            SetError(reported, "the material is not active in this scene");
            return false;
        }
        if (!ValidateSurfaceMaterialParameterOverlay(layouts, parametersJson, &reported))
            return false;

        for (StagedTarget &target : staged)
        {
            std::optional<std::string> filtered =
                FilterMaterialParameterOverlay(target.mEntry.layout, parametersJson, &reported);
            if (!filtered)
                return false;
            std::optional<std::string> resolved = OverlayMaterialParameters(target.mEntry.layout,
                target.mEntry.parametersJson, *filtered, &reported);
            if (!resolved)
                return false;
            std::optional<std::vector<std::byte>> record =
                PackMaterialRecord(target.mEntry.layout, *resolved, &reported);
            if (!record)
                return false;
            if (!ValidateRecordForPublication(target.mEntry.frontend, target.mEntry.layout, *record, reported))
                return false;

            SurfaceMaterialBinding candidate = *target.mEntry.binding;
            SetShadingValuesFromRecord(candidate, target.mEntry.layout, *record);
            target.mChanged = target.mSchemaChanged || *record != target.mBytes;
            target.mResolvedJson = std::move(*resolved);
            target.mBytes = std::move(*record);
            target.mBinding = candidate;
        }

        bool changed = false;
        std::vector<MaterialRecordUpdate> updates;
        updates.reserve(staged.size());
        std::vector<Entry *> entries;
        entries.reserve(staged.size());
        for (StagedTarget &target : staged)
        {
            changed |= target.mChanged;
            if (target.mChanged)
                updates.push_back(MaterialRecordUpdate{
                    .mMaterial = target.mEntry.binding->material, .mBytes = target.mBytes });
            const auto entry = mEntries.find(target.mKey);
            if (entry == mEntries.end())
            {
                SetError(reported, "the material entry moved while the preview was staged");
                return false;
            }
            entries.push_back(&entry->second);
            target.mEntry.binding = target.mBinding;
            target.mEntry.parametersJson = std::move(target.mResolvedJson);
        }
        if (!changed)
            return true;

        // Every candidate validated and copied; the commit itself does not
        // allocate, and only then does any cached metadata move.
        if (!mHost->PrepareMaterialRecords(updates, &reported))
            return false;
        mHost->CommitMaterialRecords();

        static_assert(std::is_nothrow_move_assignable_v<Entry>);
        for (std::size_t i = 0; i < staged.size(); ++i)
            *entries[i] = std::move(staged[i].mEntry);
        ++mGeneration;
        return true;
    }
}
