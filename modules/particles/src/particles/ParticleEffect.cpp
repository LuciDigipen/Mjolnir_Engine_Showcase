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
#include <cstring>
#include <span>
#include <stdexcept>
#include <utility>

namespace mir
{
    namespace
    {
        using Json = nlohmann::json;

        struct ModuleDefinition
        {
            std::string_view name;
            bool spawn;
            ParticleSimulationTarget target;
            std::initializer_list<std::string_view> parameters;
        };

        const ModuleDefinition kModules[]{
            {"Initialize", true, ParticleSimulationTarget::GPU, {}},
            {"Age", false, ParticleSimulationTarget::GPU, {}},
            {"Integrate", false, ParticleSimulationTarget::GPU, {}},
            {"Acceleration", false, ParticleSimulationTarget::GPU, {"x", "y", "z"}}};

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

        bool ReadModules(const Json &value, std::vector<ParticleModule> &modules)
        {
            if (!value.is_array() || value.size() > 32) return false;
            modules.clear();
            for (const auto &entry : value)
            {
                if (!KnownFields(entry, {"name", "parameters"}) || !entry.contains("name") ||
                    !entry["name"].is_string()) return false;
                ParticleModule module{entry["name"].get<std::string>(), {}};
                if (entry.contains("parameters"))
                {
                    if (!entry["parameters"].is_array() || entry["parameters"].size() > 3) return false;
                    for (const auto &parameter : entry["parameters"])
                    {
                        if (!parameter.is_number()) return false;
                        module.parameters.push_back(parameter.get<float>());
                    }
                }
                modules.push_back(std::move(module));
            }
            return true;
        }

        Json WriteModules(const std::vector<ParticleModule> &modules)
        {
            Json result = Json::array();
            for (const auto &module : modules)
                result.push_back(Json{{"name", module.name}, {"parameters", module.parameters}});
            return result;
        }

        bool ValidateModules(const ParticleEmitterAsset &emitter, std::string &error)
        {
            for (bool spawn : {true, false})
            {
                const auto &modules = spawn ? emitter.spawnModules : emitter.updateModules;
                if (modules.size() > 32)
                {
                    error = "particle module stack exceeds 32 entries";
                    return false;
                }
                for (const auto &module : modules)
                {
                    const auto definition = std::find_if(std::begin(kModules), std::end(kModules),
                        [&](const ModuleDefinition &entry) { return entry.name == module.name && entry.spawn == spawn &&
                            entry.target == emitter.simulationTarget; });
                    if (definition == std::end(kModules))
                    {
                        error = "unknown particle module or unsupported stage/target: " + module.name;
                        return false;
                    }
                    if (module.parameters.size() != definition->parameters.size() ||
                        std::any_of(module.parameters.begin(), module.parameters.end(), [](float p) { return !std::isfinite(p); }))
                    {
                        error = "invalid particle module parameters: " + module.name;
                        return false;
                    }
                }
            }
            error.clear();
            return true;
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
            if (!KnownFields(value, source ? std::initializer_list<std::string_view>{"version", "target", "lifetime", "spawnRate", "capacity", "texture", "renderer", "spawnModules", "updateModules"} :
                std::initializer_list<std::string_view>{"target", "lifetime", "spawnRate", "capacity", "textureId", "renderer", "spawnModules", "updateModules", "scriptId", "scriptLayoutId"}))
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
            if (emitter.simulationTarget == ParticleSimulationTarget::CPU)
            {
                emitter.spawnModules.clear();
                emitter.updateModules.clear();
            }
            if ((value.contains("spawnModules") && !ReadModules(value["spawnModules"], emitter.spawnModules)) ||
                (value.contains("updateModules") && !ReadModules(value["updateModules"], emitter.updateModules)))
                return Invalid<ParticleEmitterAssetResult>("invalid ordered particle module stack");
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
                if (!value.contains("scriptId") || !value["scriptId"].is_number_unsigned() ||
                    !value.contains("scriptLayoutId") || !value["scriptLayoutId"].is_number_unsigned())
                    return Invalid<ParticleEmitterAssetResult>("cooked emitter requires script and layout dependency IDs");
                emitter.script = AssetId{value["scriptId"].get<uint64_t>()};
                emitter.scriptLayout = AssetId{value["scriptLayoutId"].get<uint64_t>()};
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

    std::string ParticleScriptLayout()
    {
        return R"({"version":1,"stride":80,"binding":4,"pushConstantBytes":4,"fields":[{"name":"range","type":"uint4","offset":0},{"name":"time","type":"float4","offset":16},{"name":"velocity","type":"float4","offset":32},{"name":"color","type":"float4","offset":48},{"name":"emission","type":"float4","offset":64}]})";
    }

    std::string GenerateParticleScript(const ParticleEmitterAsset &emitter)
    {
        std::string error;
        if (!ValidateParticleEmitterAsset(emitter, error)) throw std::runtime_error(error);
        if (emitter.simulationTarget != ParticleSimulationTarget::GPU)
            throw std::runtime_error("CPU particle emitters have no GPU script");
        std::string source = R"(
struct Particle
{
    float4 positionAge;
    float4 velocityLifetime;
    float4 sizeRotation;
    float4 color;
    uint4 identity;
};
struct Parameters
{
    uint4 range;
    float4 time;
    float4 velocity;
    float4 color;
    float4 emission;
};
struct Counts
{
    uint vertices, alive, firstVertex, firstInstance;
    uint spawned, dropped, generation, reserved;
};
[[vk::binding(2)]] RWStructuredBuffer<Particle> particles;
[[vk::binding(3)]] RWStructuredBuffer<Counts> counts;
[[vk::binding(4)]] StructuredBuffer<Parameters> inputs;
struct Dispatch { uint emitterIndex; };
[[vk::push_constant]] Dispatch dispatch;
[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    uint instance = dispatch.emitterIndex;
    Parameters p = inputs[instance];
    Counts old = counts[instance];
    uint alive = 0;
    uint previous = old.generation == p.range.z && p.time.w == 0 ? min(old.alive, p.range.y) : 0;
    for (uint i = 0; i < previous; ++i)
    {
        Particle a = particles[p.range.x + i];
)";
        for (const auto &module : emitter.updateModules)
        {
            if (module.name == "Age") source += "        a.positionAge.w += p.time.x;\n";
            else if (module.name == "Integrate") source += "        a.positionAge.xyz += a.velocityLifetime.xyz * p.time.x;\n";
            else if (module.name == "Acceleration")
            {
                source += "        a.velocityLifetime.xyz += float3(";
                for (size_t i = 0; i < 3; ++i)
                {
                    if (i) source += ", ";
                    source += Json(module.parameters[i]).dump();
                }
                source += ") * p.time.x;\n";
            }
        }
        source += R"(
        if (a.positionAge.w < a.velocityLifetime.w && all(abs(a.positionAge.xyz) <= 1024.0))
            particles[p.range.x + alive++] = a;
    }
    uint spawned = min(p.range.w, p.range.y - alive);
    uint spawnBase = old.generation == p.range.z && p.time.w == 0 ? old.reserved : 0;
    for (uint i = 0; i < spawned; ++i)
    {
        Particle a = (Particle)0;
)";
        for (const auto &module : emitter.spawnModules)
            if (module.name == "Initialize")
                source += R"(
        a.positionAge = float4(p.emission.xyz, 0);
        a.velocityLifetime = float4(p.velocity.xyz, p.time.y);
        a.sizeRotation = float4(p.time.z, p.time.z, 0, 0);
        a.color = p.color;
)";
        source += R"(
        a.identity = uint4(instance, p.range.z, spawnBase + i, 0);
        particles[p.range.x + alive + i] = a;
    }
    Counts c;
    c.vertices = 6;
    c.alive = alive + spawned;
    c.firstVertex = 0;
    c.firstInstance = 0;
    c.spawned = spawned;
    c.dropped = p.range.w - spawned;
    c.generation = p.range.z;
    c.reserved = spawnBase + spawned;
    counts[instance] = c;
}
)";
        return source;
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
            return ValidateModules(emitter, error);
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
            {"textureId", emitter.texture.value}, {"renderer", WriteRenderer(emitter)},
            {"spawnModules", WriteModules(emitter.spawnModules)}, {"updateModules", WriteModules(emitter.updateModules)},
            {"scriptId", emitter.script.value}, {"scriptLayoutId", emitter.scriptLayout.value}}.dump();
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
        if (result.mEmitter->simulationTarget == ParticleSimulationTarget::GPU)
        {
            const auto *script = cache.Load(result.mEmitter->script);
            const auto *layout = cache.Load(result.mEmitter->scriptLayout);
            uint32_t magic = 0;
            if (script && script->content.size() >= 20) std::memcpy(&magic, script->content.data(), sizeof(magic));
            if (!script || script->header.typeTag != kParticleScriptTypeTag ||
                script->header.contentVersion != kParticleScriptContentVersion ||
                script->content.size() % 4 != 0 || magic != 0x07230203u ||
                !layout || layout->header.typeTag != kParticleScriptLayoutTypeTag ||
                layout->header.contentVersion != kParticleScriptLayoutContentVersion ||
                AsStringView(*layout) != ParticleScriptLayout())
                return Invalid<ParticleEmitterAssetResult>("particle script or parameter layout dependency is missing or invalid");
            result.mEmitter->scriptCode.assign(script->content.begin(), script->content.end());
        }
        return result;
    }
}
