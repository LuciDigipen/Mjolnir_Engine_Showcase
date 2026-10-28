/**
 * @file ParticleEffect.cpp
 * @author sumin.park
 * @brief Validated effect schema, blob framing and texture dependencies.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "particles/ParticleEffect.h"

#include <assets/AssetCache.h>
#include <assets/AssetPath.h>
#include <assets/ModelAsset.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <span>
#include <stdexcept>
#include <utility>

namespace mir
{
    namespace
    {
        using Json = nlohmann::json;

        ParticleEffectResult Invalid(std::string message)
        {
            ParticleEffectResult result;
            result.mError = std::move(message);
            return result;
        }

        ParticleEffectResult Parse(std::string_view text, bool source, std::string_view assetIdRoot)
        {
            const Json value = Json::parse(text, nullptr, false);
            if (!value.is_object())
                return Invalid("particle effect must be a JSON object");
            for (const auto &[key, field] : value.items())
                if (key != "lifetime" && key != "spawnRate" && key != "capacity" &&
                    key != (source ? "texture" : "textureId") && !(source && key == "version"))
                    return Invalid("unknown particle effect field: " + key);

            if (source && (!value.contains("version") || !value["version"].is_number_integer() ||
                value["version"] != 1))
                return Invalid("unsupported particle source version");
            if (!value.contains("lifetime") || !value["lifetime"].is_number() ||
                !value.contains("spawnRate") || !value["spawnRate"].is_number() ||
                !value.contains("capacity") || !value["capacity"].is_number_integer() ||
                value["capacity"] < 1 || value["capacity"] > kMaxEffectParticles)
                return Invalid("invalid or missing lifetime, spawnRate or capacity");

            ParticleEffectResult result;
            ParticleEffect effect;
            effect.lifetime = value["lifetime"].get<float>();
            effect.spawnRate = value["spawnRate"].get<float>();
            effect.capacity = value["capacity"].get<uint32_t>();
            if (source)
            {
                if (!value.contains("texture") || !value["texture"].is_string())
                    return Invalid("particle effect requires a texture path");
                const auto relative = NormalizeAssetPath(value["texture"].get_ref<const std::string &>());
                const auto logical = MakeLogicalAssetPath(assetIdRoot,
                    value["texture"].get_ref<const std::string &>());
                if (!relative.Ok() || !logical.Ok())
                    return Invalid("particle texture must be inside the game's asset root");
                effect.texture = MakeAssetId(logical.path);
                result.mTexturePath = relative.path;
            }
            else
            {
                if (!value.contains("textureId") || !value["textureId"].is_number_unsigned())
                    return Invalid("cooked particle effect requires an unsigned textureId");
                effect.texture = AssetId{value["textureId"].get<uint64_t>()};
            }
            if (!ValidateParticleEffect(effect, result.mError))
                return result;
            result.mEffect = effect;
            return result;
        }
    }

    bool ValidateParticleEffect(const ParticleEffect &effect, std::string &error)
    {
        if (!std::isfinite(effect.lifetime) || effect.lifetime <= 0 || effect.lifetime > kMaxEffectLifetime)
            error = "particle lifetime must be finite and in (0, 120]";
        else if (!std::isfinite(effect.spawnRate) || effect.spawnRate < 0 || effect.spawnRate > kMaxEffectParticles)
            error = "particle spawnRate must be finite and in [0, 65536]";
        else if (effect.capacity == 0 || effect.capacity > kMaxEffectParticles)
            error = "particle capacity must be in [1, 65536]";
        else if (effect.texture.IsNull())
            error = "particle texture dependency is null";
        else
        {
            error.clear();
            return true;
        }
        return false;
    }

    ParticleEffectResult ReadParticleEffectSource(std::string_view text, std::string_view assetIdRoot)
    {
        return Parse(text, true, assetIdRoot);
    }

    std::vector<std::byte> BuildParticleEffectAsset(const ParticleEffect &effect)
    {
        std::string error;
        if (!ValidateParticleEffect(effect, error))
            throw std::runtime_error(error);
        const std::string text = Json{{"lifetime", effect.lifetime}, {"spawnRate", effect.spawnRate},
            {"capacity", effect.capacity}, {"textureId", effect.texture.value}}.dump();
        return BuildAssetBlob(kParticleEffectTypeTag, kParticleEffectContentVersion,
            std::as_bytes(std::span(text)));
    }

    ParticleEffectResult ReadParticleEffectAsset(const AssetBlobView &blob)
    {
        if (blob.header.typeTag != kParticleEffectTypeTag ||
            blob.header.contentVersion != kParticleEffectContentVersion)
            return Invalid("unsupported particle effect type or content version");
        return Parse(AsStringView(blob), false, {});
    }

    ParticleEffectResult LoadParticleEffect(AssetCache &cache, AssetId id)
    {
        const auto *blob = cache.Load(id);
        if (blob == nullptr)
            return Invalid("particle effect is missing or its blob is corrupt");
        auto result = ReadParticleEffectAsset(*blob);
        if (!result.mEffect)
            return result;
        const auto *texture = cache.Load(result.mEffect->texture);
        TextureAsset decoded;
        std::string error;
        if (texture == nullptr || !ReadTypedAsset(*texture, decoded, error))
            return Invalid("particle texture dependency is missing or invalid: " + error);
        return result;
    }
}
