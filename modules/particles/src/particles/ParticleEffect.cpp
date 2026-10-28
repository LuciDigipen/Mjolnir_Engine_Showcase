/**
 * @file ParticleEffect.cpp
 * @author sumin.park
 * @brief Validated system/emitter schemas, blob framing and typed dependencies.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "particles/ParticleEffect.h"

#include <assets/AssetCache.h>
#include <assets/AssetPath.h>
#include <assets/ModelAsset.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <span>
#include <stdexcept>
#include <utility>

namespace mir
{
    namespace
    {
        using Json = nlohmann::json;

        template <typename Result>
        Result Invalid(std::string message)
        {
            Result result;
            result.mError = std::move(message);
            return result;
        }

        bool KnownFields(const Json &value, std::initializer_list<std::string_view> names)
        {
            if (!value.is_object())
                return false;
            for (const auto &[key, field] : value.items())
                if (std::find(names.begin(), names.end(), key) == names.end())
                    return false;
            return true;
        }

        bool ReadParameters(const Json &value, ParticleUserParameters &parameters, std::string &error)
        {
            if (!KnownFields(value, {"spawnRateScale", "sizeScale"}))
            {
                error = "particle parameters must contain only spawnRateScale and sizeScale";
                return false;
            }
            for (const auto &[key, field] : value.items())
                if (!field.is_number())
                {
                    error = "particle parameter must be a number: " + key;
                    return false;
                }
            if (value.contains("spawnRateScale")) parameters.spawnRateScale = value["spawnRateScale"].get<float>();
            if (value.contains("sizeScale")) parameters.sizeScale = value["sizeScale"].get<float>();
            return ValidateParticleUserParameters(parameters, error);
        }

        Json WriteParameters(const ParticleUserParameters &parameters)
        {
            return Json{{"spawnRateScale", parameters.spawnRateScale}, {"sizeScale", parameters.sizeScale}};
        }

        template <typename Vector>
        bool ReadVector(const Json &value, Vector &vector)
        {
            if (!value.is_array() || value.size() != vector.length())
                return false;
            for (int i = 0; i < vector.length(); ++i)
            {
                if (!value[i].is_number()) return false;
                vector[i] = value[i].template get<float>();
            }
            return true;
        }

        bool ReadRenderer(const Json &value, ParticleEmitterAsset &emitter)
        {
            if (!KnownFields(value, {"size", "color", "velocity"})) return false;
            if (value.contains("size"))
            {
                if (!value["size"].is_number()) return false;
                emitter.size = value["size"].get<float>();
            }
            return (!value.contains("color") || ReadVector(value["color"], emitter.color)) &&
                (!value.contains("velocity") || ReadVector(value["velocity"], emitter.velocity));
        }

        Json WriteRenderer(const ParticleEmitterAsset &emitter)
        {
            return Json{{"size", emitter.size},
                {"color", {emitter.color.r, emitter.color.g, emitter.color.b, emitter.color.a}},
                {"velocity", {emitter.velocity.x, emitter.velocity.y, emitter.velocity.z}}};
        }

        ParticleEffectResult ParseSystem(std::string_view text, bool source, std::string_view assetIdRoot)
        {
            const Json value = Json::parse(text, nullptr, false);
            if (!KnownFields(value, source ? std::initializer_list<std::string_view>{"version", "emitters", "parameters"} :
                std::initializer_list<std::string_view>{"emitterIds", "parameters"}))
                return Invalid<ParticleEffectResult>("particle system must be an object with only the supported fields");
            if (source && (!value.contains("version") || !value["version"].is_number_integer() || value["version"] != 2))
                return Invalid<ParticleEffectResult>("unsupported particle system source version");
            const auto key = source ? "emitters" : "emitterIds";
            if (!value.contains(key) || !value[key].is_array() || value[key].empty() || value[key].size() > kMaxSystemEmitters)
                return Invalid<ParticleEffectResult>("particle system requires 1 to 8 emitter references");
            ParticleEffectResult result;
            ParticleEffect effect;
            if (value.contains("parameters") && !ReadParameters(value["parameters"], effect.parameters, result.mError))
                return result;
            for (const auto &reference : value[key])
            {
                if (source)
                {
                    if (!reference.is_string())
                        return Invalid<ParticleEffectResult>("particle emitter reference must be a path string");
                    const auto &path = reference.get_ref<const std::string &>();
                    const auto relative = NormalizeAssetPath(path);
                    const auto logical = MakeLogicalAssetPath(assetIdRoot, path);
                    if (!relative.Ok() || !logical.Ok() || !relative.path.ends_with(".emitter.json"))
                        return Invalid<ParticleEffectResult>("particle emitter must be an .emitter.json inside the game's asset root");
                    effect.emitters.push_back(MakeAssetId(logical.path));
                    result.mEmitterPaths.push_back(relative.path);
                }
                else
                {
                    if (!reference.is_number_unsigned())
                        return Invalid<ParticleEffectResult>("cooked particle emitter reference must be an unsigned asset ID");
                    effect.emitters.push_back(AssetId{reference.get<uint64_t>()});
                }
            }
            if (!ValidateParticleEffect(effect, result.mError)) return result;
            result.mEffect = std::move(effect);
            return result;
        }

        ParticleEmitterAssetResult ParseEmitter(std::string_view text, bool source, std::string_view assetIdRoot)
        {
            const Json value = Json::parse(text, nullptr, false);
            if (!KnownFields(value, source ? std::initializer_list<std::string_view>{"version", "target", "lifetime", "spawnRate", "capacity", "texture", "renderer"} :
                std::initializer_list<std::string_view>{"target", "lifetime", "spawnRate", "capacity", "textureId", "renderer"}))
                return Invalid<ParticleEmitterAssetResult>("particle emitter must be an object with only the supported fields");
            if (source && (!value.contains("version") || !value["version"].is_number_integer() || value["version"] != 1))
                return Invalid<ParticleEmitterAssetResult>("unsupported particle emitter source version");
            if (!value.contains("target") || !value["target"].is_string() ||
                (value["target"] != "gpu" && value["target"] != "cpu"))
                return Invalid<ParticleEmitterAssetResult>("particle emitter target must be gpu or cpu");
            if (!value.contains("lifetime") || !value["lifetime"].is_number() ||
                !value.contains("spawnRate") || !value["spawnRate"].is_number() ||
                !value.contains("capacity") || !value["capacity"].is_number_integer() ||
                value["capacity"] < 1 || value["capacity"] > kMaxEffectParticles)
                return Invalid<ParticleEmitterAssetResult>("invalid or missing lifetime, spawnRate or capacity");
            ParticleEmitterAssetResult result;
            ParticleEmitterAsset emitter;
            emitter.simulationTarget = value["target"] == "gpu" ? ParticleSimulationTarget::GPU : ParticleSimulationTarget::CPU;
            emitter.lifetime = value["lifetime"].get<float>();
            emitter.spawnRate = value["spawnRate"].get<float>();
            emitter.capacity = value["capacity"].get<uint32_t>();
            if (value.contains("renderer") && !ReadRenderer(value["renderer"], emitter))
                return Invalid<ParticleEmitterAssetResult>("invalid particle sprite renderer properties");
            if (source)
            {
                if (!value.contains("texture") || !value["texture"].is_string())
                    return Invalid<ParticleEmitterAssetResult>("particle emitter requires a texture path");
                const auto &path = value["texture"].get_ref<const std::string &>();
                const auto relative = NormalizeAssetPath(path);
                const auto logical = MakeLogicalAssetPath(assetIdRoot, path);
                if (!relative.Ok() || !logical.Ok())
                    return Invalid<ParticleEmitterAssetResult>("particle texture must be inside the game's asset root");
                emitter.texture = MakeAssetId(logical.path);
                result.mTexturePath = relative.path;
            }
            else
            {
                if (!value.contains("textureId") || !value["textureId"].is_number_unsigned())
                    return Invalid<ParticleEmitterAssetResult>("cooked particle emitter requires an unsigned textureId");
                emitter.texture = AssetId{value["textureId"].get<uint64_t>()};
            }
            if (!ValidateParticleEmitterAsset(emitter, result.mError)) return result;
            result.mEmitter = emitter;
            return result;
        }
    }

    bool ValidateParticleUserParameters(const ParticleUserParameters &parameters, std::string &error)
    {
        if (!std::isfinite(parameters.spawnRateScale) || parameters.spawnRateScale < 0 || parameters.spawnRateScale > 16 ||
            !std::isfinite(parameters.sizeScale) || parameters.sizeScale < 0 || parameters.sizeScale > 16)
        {
            error = "particle parameter scales must be finite and in [0, 16]";
            return false;
        }
        error.clear();
        return true;
    }

    bool ValidateParticleEmitterAsset(const ParticleEmitterAsset &emitter, std::string &error)
    {
        if (!std::isfinite(emitter.lifetime) || emitter.lifetime <= 0 || emitter.lifetime > kMaxEffectLifetime)
            error = "particle lifetime must be finite and in (0, 120]";
        else if (!std::isfinite(emitter.spawnRate) || emitter.spawnRate < 0 || emitter.spawnRate > kMaxEffectParticles)
            error = "particle spawnRate must be finite and in [0, 65536]";
        else if (emitter.capacity == 0 || emitter.capacity > kMaxEffectParticles)
            error = "particle capacity must be in [1, 65536]";
        else if (emitter.texture.IsNull())
            error = "particle texture dependency is null";
        else if (emitter.simulationTarget != ParticleSimulationTarget::GPU && emitter.simulationTarget != ParticleSimulationTarget::CPU)
            error = "unsupported particle simulation target";
        else if (!std::isfinite(emitter.size) || emitter.size <= 0 || emitter.size > 64)
            error = "particle sprite size must be finite and in (0, 64]";
        else
        {
            for (int i = 0; i < 4; ++i)
                if (!std::isfinite(emitter.color[i]) || emitter.color[i] < 0 ||
                    (i == 3 && emitter.color[i] > 1))
                {
                    error = "particle color must be finite and nonnegative, alpha must be in [0, 1]";
                    return false;
                }
            for (int i = 0; i < 3; ++i)
                if (!std::isfinite(emitter.velocity[i]))
                {
                    error = "particle velocity must be finite";
                    return false;
                }
            error.clear();
            return true;
        }
        return false;
    }

    bool ValidateParticleEffect(const ParticleEffect &effect, std::string &error)
    {
        if (!ValidateParticleUserParameters(effect.parameters, error)) return false;
        if (effect.emitters.empty() || effect.emitters.size() > kMaxSystemEmitters)
        {
            error = "particle system requires 1 to 8 emitter references";
            return false;
        }
        for (size_t i = 0; i < effect.emitters.size(); ++i)
            if (effect.emitters[i].IsNull() || std::find(effect.emitters.begin(), effect.emitters.begin() + i,
                effect.emitters[i]) != effect.emitters.begin() + i)
            {
                error = "particle system emitter references must be nonnull and unique";
                return false;
            }
        if (!effect.emitterDefinitions.empty() && effect.emitterDefinitions.size() != effect.emitters.size())
        {
            error = "particle system emitter definitions must match its references";
            return false;
        }
        for (const auto &emitter : effect.emitterDefinitions)
            if (!ValidateParticleEmitterAsset(emitter, error)) return false;
        error.clear();
        return true;
    }

    ParticleEffectResult ReadParticleEffectSource(std::string_view text, std::string_view assetIdRoot)
    {
        return ParseSystem(text, true, assetIdRoot);
    }

    std::vector<std::byte> BuildParticleEffectAsset(const ParticleEffect &effect)
    {
        std::string error;
        if (!ValidateParticleEffect(effect, error)) throw std::runtime_error(error);
        Json ids = Json::array();
        for (auto id : effect.emitters) ids.push_back(id.value);
        const auto text = Json{{"emitterIds", ids}, {"parameters", WriteParameters(effect.parameters)}}.dump();
        return BuildAssetBlob(kParticleEffectTypeTag, kParticleEffectContentVersion, std::as_bytes(std::span(text)));
    }

    ParticleEffectResult ReadParticleEffectAsset(const AssetBlobView &blob)
    {
        if (blob.header.typeTag != kParticleEffectTypeTag || blob.header.contentVersion != kParticleEffectContentVersion)
            return Invalid<ParticleEffectResult>("unsupported particle system type or content version");
        return ParseSystem(AsStringView(blob), false, {});
    }

    ParticleEffectResult LoadParticleEffect(AssetCache &cache, AssetId id)
    {
        const auto *blob = cache.Load(id);
        if (blob == nullptr) return Invalid<ParticleEffectResult>("particle system is missing or its blob is corrupt");
        auto result = ReadParticleEffectAsset(*blob);
        if (!result.mEffect) return result;
        for (auto emitterId : result.mEffect->emitters)
        {
            auto emitter = LoadParticleEmitterAsset(cache, emitterId);
            if (!emitter.mEmitter) return Invalid<ParticleEffectResult>("particle emitter dependency is missing or invalid: " + emitter.mError);
            result.mEffect->emitterDefinitions.push_back(*emitter.mEmitter);
        }
        return result;
    }

    ParticleEmitterAssetResult ReadParticleEmitterSource(std::string_view text, std::string_view assetIdRoot)
    {
        return ParseEmitter(text, true, assetIdRoot);
    }

    std::vector<std::byte> BuildParticleEmitterAsset(const ParticleEmitterAsset &emitter)
    {
        std::string error;
        if (!ValidateParticleEmitterAsset(emitter, error)) throw std::runtime_error(error);
        const auto text = Json{{"target", emitter.simulationTarget == ParticleSimulationTarget::GPU ? "gpu" : "cpu"},
            {"lifetime", emitter.lifetime}, {"spawnRate", emitter.spawnRate}, {"capacity", emitter.capacity},
            {"textureId", emitter.texture.value}, {"renderer", WriteRenderer(emitter)}}.dump();
        return BuildAssetBlob(kParticleEmitterTypeTag, kParticleEmitterContentVersion, std::as_bytes(std::span(text)));
    }

    ParticleEmitterAssetResult ReadParticleEmitterAsset(const AssetBlobView &blob)
    {
        if (blob.header.typeTag != kParticleEmitterTypeTag || blob.header.contentVersion != kParticleEmitterContentVersion)
            return Invalid<ParticleEmitterAssetResult>("unsupported particle emitter type or content version");
        return ParseEmitter(AsStringView(blob), false, {});
    }

    ParticleEmitterAssetResult LoadParticleEmitterAsset(AssetCache &cache, AssetId id)
    {
        const auto *blob = cache.Load(id);
        if (blob == nullptr) return Invalid<ParticleEmitterAssetResult>("particle emitter is missing or its blob is corrupt");
        auto result = ReadParticleEmitterAsset(*blob);
        if (!result.mEmitter) return result;
        const auto *texture = cache.Load(result.mEmitter->texture);
        TextureAsset decoded;
        std::string error;
        if (texture == nullptr || !ReadTypedAsset(*texture, decoded, error))
            return Invalid<ParticleEmitterAssetResult>("particle texture dependency is missing or invalid: " + error);
        return result;
    }
}
