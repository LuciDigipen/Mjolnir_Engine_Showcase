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

#include <glm/common.hpp>
#include <glm/vector_relational.hpp>
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
            {"RibbonSpawn", true, ParticleSimulationTarget::CPU, {"maxInterval"}},
            {"RibbonAge", false, ParticleSimulationTarget::CPU, {}},
            {"RibbonWind", false, ParticleSimulationTarget::CPU, {}},
            {"RibbonWidth", false, ParticleSimulationTarget::CPU, {"endScale"}},
            {"RibbonFade", false, ParticleSimulationTarget::CPU, {}},
            {"Initialize", true, ParticleSimulationTarget::GPU, {}},
            {"VelocityVariation", true, ParticleSimulationTarget::GPU, {"x", "y", "z"}},
            {"Age", false, ParticleSimulationTarget::GPU, {}},
            {"Integrate", false, ParticleSimulationTarget::GPU, {}},
            {"Acceleration", false, ParticleSimulationTarget::GPU, {"x", "y", "z"}},
            {"Drag", false, ParticleSimulationTarget::GPU, {"coefficient"}},
            {"SizeOverLife", false, ParticleSimulationTarget::GPU, {"endScale"}},
            {"FadeOverLife", false, ParticleSimulationTarget::GPU, {"endAlpha"}},
            {"Flipbook", false, ParticleSimulationTarget::GPU, {"columns", "rows", "fps"}}};

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
            if (!KnownFields(value, {"spawnRateScale", "sizeScale", "wind"}))
            {
                error = "particle parameters must contain only spawnRateScale, sizeScale and wind";
                return false;
            }
            for (const auto &[key, field] : value.items())
                if (key != "wind" && !field.is_number())
                {
                    error = "particle parameter must be a number: " + key;
                    return false;
                }
            if (value.contains("spawnRateScale")) parameters.spawnRateScale = value["spawnRateScale"].get<float>();
            if (value.contains("sizeScale")) parameters.sizeScale = value["sizeScale"].get<float>();
            if (value.contains("wind"))
            {
                const auto &wind = value["wind"];
                if (!wind.is_array() || wind.size() != 3 ||
                    std::any_of(wind.begin(), wind.end(), [](const auto &item) { return !item.is_number(); }))
                {
                    error = "particle wind must be a numeric three-vector";
                    return false;
                }
                for (int i = 0; i < 3; ++i) parameters.wind[i] = wind[i].get<float>();
            }
            return ValidateParticleUserParameters(parameters, error);
        }

        Json WriteParameters(const ParticleUserParameters &parameters)
        {
            return Json{{"spawnRateScale", parameters.spawnRateScale}, {"sizeScale", parameters.sizeScale},
                {"wind", {parameters.wind.x, parameters.wind.y, parameters.wind.z}}};
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
            if (!KnownFields(value, {"size", "color", "velocity", "alignment", "facing", "blend", "sort", "softDistance", "maxDistance", "lighting", "fog", "textured"})) return false;
            if (value.contains("size"))
            {
                if (!value["size"].is_number()) return false;
                emitter.size = value["size"].get<float>();
            }
            auto choice = [&](const char *key, const char *zero, const char *one, uint32_t &out)
            {
                if (!value.contains(key)) return true;
                if (value[key] == zero) { out = 0; return true; }
                if (value[key] == one) { out = 1; return true; }
                return false;
            };
            auto &r = emitter.renderer;
            if (!choice("alignment", "screen", "velocity", r.alignment) ||
                !choice("facing", "plane", "position", r.facing) ||
                !choice("blend", "alpha", "additive", r.blend) ||
                !choice("sort", "backToFront", "none", r.sort)) return false;
            for (auto [key, target] : {std::pair{"softDistance", &r.softDistance}, std::pair{"maxDistance", &r.maxDistance}})
                if (value.contains(key))
                {
                    if (!value[key].is_number()) return false;
                    *target = value[key].get<float>();
                }
            for (auto [key, target] : {std::pair{"fog", &r.fog}, std::pair{"textured", &r.textured}})
                if (value.contains(key))
                {
                    if (!value[key].is_boolean()) return false;
                    *target = value[key].get<bool>();
                }
            uint32_t lighting = 0;
            if (!choice("lighting", "unlit", "sunAmbient", lighting)) return false;
            r.lit = lighting != 0;
            return (!value.contains("color") || ReadVector(value["color"], emitter.color)) &&
                (!value.contains("velocity") || ReadVector(value["velocity"], emitter.velocity));
        }

        Json WriteRenderer(const ParticleEmitterAsset &emitter)
        {
            const auto &r = emitter.renderer;
            return Json{{"alignment", r.alignment ? "velocity" : "screen"}, {"facing", r.facing ? "position" : "plane"},
                {"blend", r.blend ? "additive" : "alpha"}, {"sort", r.sort ? "none" : "backToFront"},
                {"softDistance", r.softDistance}, {"maxDistance", r.maxDistance},
                {"lighting", r.lit ? "sunAmbient" : "unlit"}, {"fog", r.fog}, {"textured", r.textured},
                {"size", emitter.size},
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
                    const bool invalidValues =
                        (module.name == "RibbonSpawn" && module.parameters.size() == 1 && (module.parameters[0] < 1.0f / 60 || module.parameters[0] > 10)) ||
                        (module.name == "RibbonWidth" && module.parameters.size() == 1 && (module.parameters[0] < 0 || module.parameters[0] > 64)) ||
                        (module.name == "Drag" && module.parameters.size() == 1 && (module.parameters[0] < 0 || module.parameters[0] > 10000)) ||
                        (module.name == "SizeOverLife" && module.parameters.size() == 1 && (module.parameters[0] < 0 || module.parameters[0] > 64)) ||
                        (module.name == "FadeOverLife" && module.parameters.size() == 1 && (module.parameters[0] < 0 || module.parameters[0] > 1)) ||
                        (module.name == "VelocityVariation" && std::any_of(module.parameters.begin(), module.parameters.end(),
                            [](float p) { return p < 0 || p > 4096; })) ||
                        (module.name == "Flipbook" && module.parameters.size() == 3 &&
                            (module.parameters[0] < 1 || module.parameters[0] > 64 || std::floor(module.parameters[0]) != module.parameters[0] ||
                             module.parameters[1] < 1 || module.parameters[1] > 64 || std::floor(module.parameters[1]) != module.parameters[1] ||
                             module.parameters[2] < 0 || module.parameters[2] > 240));
                    if (module.parameters.size() != definition->parameters.size() || invalidValues ||
                        std::any_of(module.parameters.begin(), module.parameters.end(), [](float p) { return !std::isfinite(p); }))
                    {
                        error = "invalid particle module parameters: " + module.name;
                        return false;
                    }
                }
            }
            if (emitter.simulationTarget == ParticleSimulationTarget::CPU &&
                (emitter.capacity > 1024 || emitter.spawnRate != 0 || emitter.burst != 0 ||
                 emitter.spawnModules.size() != 1 || emitter.spawnModules[0].name != "RibbonSpawn" ||
                 emitter.updateModules.size() != 4 || emitter.updateModules[0].name != "RibbonAge" ||
                 emitter.updateModules[1].name != "RibbonWind" || emitter.updateModules[2].name != "RibbonWidth" ||
                 emitter.updateModules[3].name != "RibbonFade"))
            {
                error = "CPU ribbons require capacity <= 1024, zero spawnRate/burst, RibbonSpawn and RibbonAge/Wind/Width/Fade";
                return false;
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
            if (!KnownFields(value, source ? std::initializer_list<std::string_view>{"version", "target", "lifetime", "spawnRate", "capacity", "texture", "renderer", "spawnModules", "updateModules", "localOffset", "spawnPerMeter", "burst", "seed", "inheritVelocity", "teleportDistance"} :
                std::initializer_list<std::string_view>{"target", "lifetime", "spawnRate", "capacity", "textureId", "renderer", "spawnModules", "updateModules", "scriptId", "scriptLayoutId", "localOffset", "spawnPerMeter", "burst", "seed", "inheritVelocity", "teleportDistance"}))
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
            if (value.contains("localOffset") && !ReadVector(value["localOffset"], emitter.localOffset))
                return Invalid<ParticleEmitterAssetResult>("invalid particle attachment offset");
            for (const auto key : {"spawnPerMeter", "inheritVelocity", "teleportDistance"})
                if (value.contains(key) && !value[key].is_number())
                    return Invalid<ParticleEmitterAssetResult>("particle motion parameter must be numeric");
            for (const auto key : {"burst", "seed"})
                if (value.contains(key) && (!value[key].is_number_integer() || value[key] < 0 || value[key] > UINT32_MAX))
                    return Invalid<ParticleEmitterAssetResult>("particle burst/seed must be an unsigned 32-bit integer");
            if (value.contains("spawnPerMeter")) emitter.spawnPerMeter = value["spawnPerMeter"].get<float>();
            if (value.contains("burst")) emitter.burst = value["burst"].get<uint32_t>();
            if (value.contains("seed")) emitter.seed = value["seed"].get<uint32_t>();
            if (value.contains("inheritVelocity")) emitter.inheritVelocity = value["inheritVelocity"].get<float>();
            if (value.contains("teleportDistance")) emitter.teleportDistance = value["teleportDistance"].get<float>();
            if (value.contains("renderer") && !ReadRenderer(value["renderer"], emitter))
                return Invalid<ParticleEmitterAssetResult>("invalid particle renderer properties");
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
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(parameters.wind[i]) || std::abs(parameters.wind[i]) > 4096)
            {
                error = "particle wind must be finite and within 4096 metres per second";
                return false;
            }
        error.clear();
        return true;
    }

    std::string ParticleScriptLayout()
    {
        return R"({"version":2,"stride":160,"binding":4,"birthBinding":6,"birthStride":16,"pushConstantBytes":4,"fields":[{"name":"range","type":"uint4","offset":0},{"name":"time","type":"float4","offset":16},{"name":"velocity","type":"float4","offset":32},{"name":"color","type":"float4","offset":48},{"name":"emission","type":"float4","offset":64},{"name":"emissionStart","type":"float4","offset":80},{"name":"attachmentVelocity","type":"float4","offset":96},{"name":"tileDelta","type":"float4","offset":112},{"name":"wind","type":"float4","offset":128},{"name":"birthRange","type":"uint4","offset":144}]})";
    }

    std::string GenerateParticleScript(const ParticleEmitterAsset &emitter)
    {
        std::string error;
        if (!ValidateParticleEmitterAsset(emitter, error)) throw std::runtime_error(error);
        if (emitter.simulationTarget != ParticleSimulationTarget::GPU)
            throw std::runtime_error("CPU particle emitters have no GPU script");
        const auto number = [](float value) { return Json(value).dump(); };
        const auto vector = [&](const ParticleModule &module)
        {
            return "float3(" + number(module.parameters[0]) + ", " + number(module.parameters[1]) + ", " +
                number(module.parameters[2]) + ")";
        };
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
    float4 emissionStart;
    float4 attachmentVelocity;
    float4 tileDelta;
    float4 wind;
    uint4 birthRange;
};
struct Birth
{
    float fraction, age;
    uint sequence, reserved;
};
struct Counts
{
    uint vertices, alive, firstVertex, firstInstance;
    uint spawned, dropped, generation, reserved;
};
[[vk::binding(2)]] RWStructuredBuffer<Particle> particles;
[[vk::binding(3)]] RWStructuredBuffer<Counts> counts;
[[vk::binding(4)]] StructuredBuffer<Parameters> inputs;
[[vk::binding(6)]] StructuredBuffer<Birth> births;
uint ParticleHash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}
float SignedNoise(uint value)
{
    return float(ParticleHash(value) >> 8) * (2.0 / 16777215.0) - 1.0;
}
void UpdateParticle(inout Particle a, Parameters p, float dt)
{
)";
        for (const auto &module : emitter.updateModules)
        {
            if (module.name == "Age") source += "    a.positionAge.w += dt;\n";
            else if (module.name == "Integrate") source += "    a.positionAge.xyz += a.velocityLifetime.xyz * dt;\n";
            else if (module.name == "Acceleration")
                source += "    a.velocityLifetime.xyz += " + vector(module) + " * dt;\n";
            else if (module.name == "Drag")
                source += "    a.velocityLifetime.xyz += (p.wind.xyz - a.velocityLifetime.xyz) * (1 - exp(-" +
                    number(module.parameters[0]) + " * dt));\n";
            else if (module.name == "SizeOverLife")
                source += "    a.sizeRotation.xy = p.time.zz * lerp(1.0, " + number(module.parameters[0]) +
                    ", saturate(a.positionAge.w / a.velocityLifetime.w));\n";
            else if (module.name == "FadeOverLife")
                source += "    a.color.a = lerp(p.color.a, " + number(module.parameters[0]) +
                    ", saturate(a.positionAge.w / a.velocityLifetime.w));\n";
            else if (module.name == "Flipbook")
            {
                const uint32_t columns = static_cast<uint32_t>(module.parameters[0]);
                const uint32_t rows = static_cast<uint32_t>(module.parameters[1]);
                source += "    a.sizeRotation.z = fmod(floor(a.positionAge.w * " + number(module.parameters[2]) + "), " +
                    std::to_string(columns * rows) + ".0);\n";
                source += "    a.sizeRotation.w = asfloat(" + std::to_string(columns | (rows << 16)) + "u);\n";
            }
        }
        source += R"(
}
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
        a.positionAge.xyz += p.tileDelta.xyz;
        UpdateParticle(a, p, p.time.x);
        if (a.positionAge.w < a.velocityLifetime.w && all(isfinite(a.positionAge)) && all(abs(a.positionAge.xyz) <= 65536.0))
            particles[p.range.x + alive++] = a;
    }
    uint spawned = min(p.range.w, p.range.y - alive);
    uint spawnBase = old.generation == p.range.z && p.time.w == 0 ? old.reserved : 0;
    for (uint i = 0; i < spawned; ++i)
    {
        Birth birth;
        birth.fraction = 1;
        birth.age = 0;
        birth.sequence = spawnBase + i;
        birth.reserved = 0;
        if (i < p.birthRange.y) birth = births[p.birthRange.x + i];
        uint randomSeed = birth.sequence ^ p.birthRange.z;
        Particle a = (Particle)0;
)";
        for (const auto &module : emitter.spawnModules)
        {
            if (module.name == "Initialize")
                source += R"(
        a.positionAge = float4(lerp(p.emissionStart.xyz, p.emission.xyz, birth.fraction), 0);
        a.velocityLifetime = float4(p.velocity.xyz + p.attachmentVelocity.xyz * p.emissionStart.w, p.time.y);
        a.sizeRotation = float4(p.time.z, p.time.z, 0, 0);
        a.color = p.color;
)";
            else if (module.name == "VelocityVariation")
                source += "        a.velocityLifetime.xyz += " + vector(module) +
                    " * float3(SignedNoise(randomSeed), SignedNoise(randomSeed ^ 0x9e3779b9u), SignedNoise(randomSeed ^ 0x85ebca6bu));\n";
        }
        const bool customFade = std::any_of(emitter.updateModules.begin(), emitter.updateModules.end(),
            [](const ParticleModule &module) { return module.name == "FadeOverLife"; });
        source += "        a.identity = uint4(instance, p.range.z, birth.sequence, " + std::to_string(customFade ? 1 : 0) + "u);\n";
        source += R"(
        UpdateParticle(a, p, birth.age);
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
    c.reserved = spawnBase + p.range.w;
    counts[instance] = c;
}
)";
        return source;
    }

    bool ValidateParticleEmitterAsset(const ParticleEmitterAsset &emitter, std::string &error)
    {
        const auto &r = emitter.renderer;
        if (r.alignment > 1 || r.facing > 1 || r.blend > 1 || r.sort > 1 ||
            !std::isfinite(r.softDistance) || r.softDistance < 0 || r.softDistance > 1024 ||
            !std::isfinite(r.maxDistance) || r.maxDistance <= 0 || r.maxDistance > 10000000)
        {
            error = "particle renderer choices or finite distance bounds are invalid";
            return false;
        }
        if (!std::isfinite(emitter.lifetime) || emitter.lifetime <= 0 || emitter.lifetime > kMaxEffectLifetime)
            error = "particle lifetime must be finite and in (0, 120]";
        else if (!std::isfinite(emitter.spawnRate) || emitter.spawnRate < 0 || emitter.spawnRate > kMaxEffectParticles)
            error = "particle spawnRate must be finite and in [0, 65536]";
        else if (emitter.capacity == 0 || emitter.capacity > kMaxEffectParticles)
            error = "particle capacity must be in [1, 65536]";
        else if (!std::isfinite(emitter.spawnPerMeter) || emitter.spawnPerMeter < 0 || emitter.spawnPerMeter > kMaxEffectParticles ||
            emitter.burst > kMaxEffectParticles || !std::isfinite(emitter.inheritVelocity) || emitter.inheritVelocity < 0 || emitter.inheritVelocity > 16 ||
            !std::isfinite(emitter.teleportDistance) || emitter.teleportDistance <= 0 || emitter.teleportDistance > 65536 ||
            !std::isfinite(emitter.localOffset.x) || !std::isfinite(emitter.localOffset.y) || !std::isfinite(emitter.localOffset.z) ||
            glm::any(glm::greaterThan(glm::abs(emitter.localOffset), glm::vec3(4096))))
            error = "particle motion parameters exceed finite supported ranges";
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
            {"localOffset", {emitter.localOffset.x, emitter.localOffset.y, emitter.localOffset.z}},
            {"spawnPerMeter", emitter.spawnPerMeter}, {"burst", emitter.burst}, {"seed", emitter.seed},
            {"inheritVelocity", emitter.inheritVelocity}, {"teleportDistance", emitter.teleportDistance},
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
        if (decoded.width > 2048 || decoded.height > 2048 || decoded.rgba.size() > 16 * 1024 * 1024)
            return Invalid<ParticleEmitterAssetResult>("particle texture exceeds 2048 pixels per side or 16 MiB");
        result.mEmitter->textureWidth = decoded.width;
        result.mEmitter->textureHeight = decoded.height;
        result.mEmitter->texturePixels = std::move(decoded.rgba);
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
