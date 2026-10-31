/**
 * @file ParticleEffect.tests.cpp
 * @author sumin.park
 * @brief Effect schema, cooked version and dependency validation.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ParticleTestAssets.h"

#include <assets/AssetCache.h>
#include <particles/ParticleEffect.h>
#include <particles/ParticleRuntime.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <limits>
#include <tuple>
#include <utility>

TEST_CASE("Particle system source cooks emitter references and user parameter defaults", "[particles]")
{
    const auto source = mir::ReadParticleEffectSource(
        R"({"version":3,"placements":[{"id":1,"emitter":"effects/exhaust.emitter.json","offset":[0,0,0]}],"parameters":{"spawnRateScale":2,"sizeScale":0.5}})", "assets");
    REQUIRE(source.mEffect);
    REQUIRE(source.mEffect->emitters.size() == 1);
    CHECK(source.mEffect->emitters[0] == mir::test::ParticleTestAssets::EmitterId());
    REQUIRE(source.mEmitterPaths.size() == 1);
    CHECK(source.mEmitterPaths[0] == "effects/exhaust.emitter.json");
    const auto bytes = mir::BuildParticleEffectAsset(*source.mEffect);
    const auto blob = mir::ParseAssetBlob(bytes);
    REQUIRE(blob);
    const auto cooked = mir::ReadParticleEffectAsset(*blob);
    REQUIRE(cooked.mEffect);
    CHECK(cooked.mEffect->emitters == source.mEffect->emitters);
    CHECK(cooked.mEffect->parameters.spawnRateScale == 2.0f);
    CHECK(cooked.mEffect->parameters.sizeScale == 0.5f);
    CHECK(cooked.mEffect->emitterDefinitions.empty());
}

TEST_CASE("Particle emitter source cooks renderer properties and a typed texture reference", "[particles]")
{
    const auto source = mir::ReadParticleEmitterSource(
        R"({"version":3,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"textures/white.png",
            "renderer":{"size":0.25,"color":[1,0.5,0.25,0.5],"velocity":[0,2,0]}})", "assets");
    REQUIRE(source.mEmitter);
    CHECK(source.mEmitter->texture == mir::test::ParticleTestAssets::TextureId());
    CHECK(source.mTexturePath == "textures/white.png");
    const auto bytes = mir::BuildParticleEmitterAsset(*source.mEmitter);
    const auto blob = mir::ParseAssetBlob(bytes);
    REQUIRE(blob);
    const auto cooked = mir::ReadParticleEmitterAsset(*blob);
    REQUIRE(cooked.mEmitter);
    CHECK(cooked.mEmitter->lifetime == 2.0f);
    CHECK(cooked.mEmitter->spawnRate == 60.0f);
    CHECK(cooked.mEmitter->capacity == 256);
    CHECK(cooked.mEmitter->texture == source.mEmitter->texture);
    CHECK(cooked.mEmitter->simulationTarget == mir::ParticleSimulationTarget::GPU);
    CHECK(cooked.mEmitter->size == 0.25f);
    CHECK(cooked.mEmitter->color == source.mEmitter->color);
    CHECK(cooked.mEmitter->velocity == source.mEmitter->velocity);
}

TEST_CASE("Particle system source rejects unsupported schemas references and user parameters", "[particles]")
{
    for (const std::string_view source : {
        "[]", "{bad", "{}",
            R"({"version":1,"placements":[{"id":1,"emitter":"effects/exhaust.emitter.json","offset":[0,0,0]}]})",
            R"({"version":3,"placements":[]})",
            R"({"version":3,"placements":[{"id":1,"emitter":1,"offset":[0,0,0]}]})",
            R"({"version":3,"placements":[{"id":1,"emitter":"../outside.emitter.json","offset":[0,0,0]}]})",
            R"({"version":3,"placements":[{"id":1,"emitter":"C:/outside.emitter.json","offset":[0,0,0]}]})",
            R"({"version":3,"placements":[{"id":1,"emitter":"texture.png","offset":[0,0,0]}]})",
            R"({"version":3,"placements":[{"id":1,"emitter":"effects/exhaust.emitter.json","offset":[0,0,0]}],"typo":1})",
            R"({"version":3,"placements":[{"id":1,"emitter":"effects/exhaust.emitter.json","offset":[0,0,0]}],"parameters":null})",
            R"({"version":3,"placements":[{"id":1,"emitter":"effects/exhaust.emitter.json","offset":[0,0,0]}],"parameters":{"spawnRateScale":-1}})",
            R"({"version":3,"placements":[{"id":1,"emitter":"effects/exhaust.emitter.json","offset":[0,0,0]}],"parameters":{"sizeScale":17}})",
            R"({"version":3,"placements":[{"id":1,"emitter":"effects/exhaust.emitter.json","offset":[0,0,0]}],"parameters":{"sizeScale":1e100}})",
            R"({"version":3,"placements":[{"id":1,"emitter":"effects/exhaust.emitter.json","offset":[0,0,0]}],"parameters":{"sizeScale":true}})",
            R"({"version":3,"placements":[{"id":1,"emitter":"effects/exhaust.emitter.json","offset":[0,0,0]}],"parameters":{"unknown":1}})"})
    {
        const auto result = mir::ReadParticleEffectSource(source, "assets");
        CHECK_FALSE(result.mEffect);
        CHECK_FALSE(result.mError.empty());
    }
}

TEST_CASE("Particle emitter source rejects unsafe paths invalid targets and malformed properties", "[particles]")
{
    for (const std::string_view source : {
        "[]", "{bad", "{}",
            R"({"version":1,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"textures/white.png"})",
            R"({"version":2,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"textures/white.png"})",
            R"({"version":3,"target":"other","lifetime":2,"spawnRate":60,"capacity":256,"texture":"textures/white.png"})",
            R"({"version":3,"target":"gpu","lifetime":-1,"spawnRate":60,"capacity":256,"texture":"textures/white.png"})",
            R"({"version":3,"target":"gpu","lifetime":121,"spawnRate":60,"capacity":256,"texture":"textures/white.png"})",
            R"({"version":3,"target":"gpu","lifetime":1e100,"spawnRate":60,"capacity":256,"texture":"textures/white.png"})",
            R"({"version":3,"target":"gpu","lifetime":2,"spawnRate":-1,"capacity":256,"texture":"textures/white.png"})",
            R"({"version":3,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":1.5,"texture":"textures/white.png"})",
            R"({"version":3,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":65537,"texture":"textures/white.png"})",
            R"({"version":3,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"../outside.png"})",
            R"({"version":3,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"C:/outside.png"})",
            R"({"version":3,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"x.png","typo":1})",
            R"({"version":3,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"x.png","renderer":{"size":0}})",
            R"({"version":3,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"x.png","renderer":{"color":[1,1,1,2]}})",
            R"({"version":3,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"x.png","renderer":{"velocity":[0,1]}})",
            R"({"version":3,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"x.png","renderer":{"velocity":[0,1e100,0]}})",
            R"({"version":3,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"x.png","renderer":{"typo":1}})"})
    {
        const auto result = mir::ReadParticleEmitterSource(source, "assets");
        CHECK_FALSE(result.mEmitter);
        CHECK_FALSE(result.mError.empty());
    }
}

TEST_CASE("Particle loading resolves typed emitter and texture dependencies atomically", "[particles]")
{
    mir::test::ParticleTestAssets files;
    auto load = [&]
        {
            const auto manifest = mir::AssetManifest::LoadFile(files.Root() / mir::kAssetManifestFileName);
            if (!manifest) throw std::runtime_error("missing test manifest");
            mir::AssetCache cache(&*manifest, files.Root());
            return mir::LoadParticleEffect(cache, files.EffectId());
        };
    const auto valid = load();
    REQUIRE(valid.mEffect);
    REQUIRE(valid.mEffect->emitterDefinitions.size() == 1);
    CHECK(valid.mEffect->emitterDefinitions[0].texture == files.TextureId());
    CHECK(valid.mEffect->emitterDefinitions[0].textureWidth == 1);
    CHECK(valid.mEffect->emitterDefinitions[0].textureHeight == 1);
    CHECK(valid.mEffect->emitterDefinitions[0].texturePixels == (std::vector<uint8_t>{255, 255, 255, 255}));

    SECTION("Missing emitter")
    {
        std::filesystem::remove(files.Root() / "emitter.blob");
        CHECK_FALSE(load().mEffect);
    }
    SECTION("Wrong emitter type")
    {
        files.Write("emitter.blob", mir::BuildAssetBlob(mir::kRawAssetTypeTag, 1, {}));
        CHECK_FALSE(load().mEffect);
    }
    SECTION("Missing texture")
    {
        files.Publish(0.05f, false);
        CHECK_FALSE(load().mEffect);
    }
    SECTION("Wrong texture type")
    {
        files.Publish(0.05f, true, false);
        CHECK_FALSE(load().mEffect);
    }
    SECTION("Oversized particle texture")
    {
        files.Write("texture.blob", mir::BuildTypedAsset(mir::TextureAsset{ 2049, 1, std::vector<uint8_t>(2049 * 4, 255) }));
        CHECK_FALSE(load().mEffect);
    }
    SECTION("Unsupported system version")
    {
        files.Publish(0.05f, true, true, 999);
        CHECK_FALSE(load().mEffect);
    }
    SECTION("Missing script")
    {
        std::filesystem::remove(files.Root() / "script.blob");
        CHECK_FALSE(load().mEffect);
    }
    SECTION("Wrong script type")
    {
        files.Write("script.blob", mir::BuildAssetBlob(mir::kRawAssetTypeTag, 1, {}));
        CHECK_FALSE(load().mEffect);
    }
    SECTION("Malformed SPIR-V")
    {
        const uint32_t words[]{ 0, 0, 0, 0, 0 };
        files.Write("script.blob", mir::BuildAssetBlob(mir::kParticleScriptTypeTag,
            mir::kParticleScriptContentVersion, std::as_bytes(std::span(words))));
        CHECK_FALSE(load().mEffect);
    }
    SECTION("Missing layout")
    {
        std::filesystem::remove(files.Root() / "layout.blob");
        CHECK_FALSE(load().mEffect);
    }
    SECTION("Wrong layout type")
    {
        files.Write("layout.blob", mir::BuildAssetBlob(mir::kRawAssetTypeTag, 1, {}));
        CHECK_FALSE(load().mEffect);
    }
    SECTION("Incompatible layout")
    {
        const std::string layout = R"({"stride":96})";
        files.Write("layout.blob", mir::BuildAssetBlob(mir::kParticleScriptLayoutTypeTag,
            mir::kParticleScriptLayoutContentVersion, std::as_bytes(std::span(layout))));
        CHECK_FALSE(load().mEffect);
    }
}

TEST_CASE("Particle behavior stacks reject unknown behaviors unsupported stages targets and parameters", "[particles]")
{
    for (const std::string_view fields : {
        R"("spawnBehaviors":[{"name":"Unknown"}])",
            R"("spawnBehaviors":[{"name":"Integrate"}])",
            R"("updateBehaviors":[{"name":"Initialize"}])",
            R"("updateBehaviors":[{"name":"Age","parameters":[1]}])",
            R"("updateBehaviors":[{"name":"Acceleration","parameters":[1,2]}])",
            R"("updateBehaviors":[{"name":"Acceleration","parameters":[1,2,1e100]}])",
            R"("updateBehaviors":[{"name":"Acceleration","parameters":[1,"2",3]}])",
            R"("updateBehaviors":[{"name":"Age","typo":0}])",
            R"("updateBehaviors":[{"parameters":[]}])",
            R"("updateBehaviors":{})"})
    {
        const auto text = std::string(R"({"version":3,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"x.png",)") +
            std::string(fields) + "}";
        const auto result = mir::ReadParticleEmitterSource(text, "assets");
        INFO(text);
        CHECK_FALSE(result.mEmitter);
        CHECK_FALSE(result.mError.empty());
    }
    const auto cpu = mir::ReadParticleEmitterSource(
        R"({"version":3,"target":"cpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"x.png","updateBehaviors":[{"name":"Age"}]})", "assets");
    CHECK_FALSE(cpu.mEmitter);
    mir::ParticleEmitterAsset emitter;
    emitter.texture = mir::MakeAssetId("assets/x.png");
    emitter.updateBehaviors = { { "Acceleration",{ 0, std::numeric_limits<float>::infinity(), 0 } } };
    std::string error;
    CHECK_FALSE(mir::ValidateParticleEmitterAsset(emitter, error));
}

TEST_CASE("Identical particle stacks share generated code independently of emitter values", "[particles]")
{
    mir::ParticleEmitterAsset first;
    first.texture = mir::MakeAssetId("assets/a.png");
    auto second = first;
    second.texture = mir::MakeAssetId("assets/b.png");
    second.lifetime = 10;
    second.capacity = 1000;
    second.spawnRate = 120;
    second.size = 2;
    second.color = { 0.5f, 0.5f, 0.5f, 0.5f };
    second.velocity = { 1, 2, 3 };
    CHECK(mir::GenerateParticleScript(first) == mir::GenerateParticleScript(second));
    second.updateBehaviors.push_back({ "Acceleration",{ 0, -1, 0 } });
    CHECK(mir::GenerateParticleScript(first) != mir::GenerateParticleScript(second));
}

TEST_CASE("Particle blob readers reject malformed type version and asset framing", "[particles]")
{
    const std::string badSystem = R"({"emitterIds":[-1],"parameters":{}})";
    const std::string badEmitter = R"({"target":"gpu","lifetime":1,"spawnRate":1,"capacity":1,"textureId":-1})";
    const std::string malformed = "{broken";
    for (const auto &[tag, version, text] :{
        std::tuple{ mir::kParticleEffectTypeTag, mir::kParticleEffectContentVersion, badSystem },
        std::tuple{ mir::kParticleEffectTypeTag, mir::kParticleEffectContentVersion, malformed } })
    {
        const auto bytes = mir::BuildAssetBlob(tag, version, std::as_bytes(std::span(text)));
        CHECK_FALSE(mir::ReadParticleEffectAsset(*mir::ParseAssetBlob(bytes)).mEffect);
    }
    const auto emitterBytes = mir::BuildAssetBlob(mir::kParticleEmitterTypeTag,
        mir::kParticleEmitterContentVersion, std::as_bytes(std::span(badEmitter)));
    CHECK_FALSE(mir::ReadParticleEmitterAsset(*mir::ParseAssetBlob(emitterBytes)).mEmitter);
    const auto wrongTypeBytes = mir::BuildAssetBlob(0, 1, {});
    CHECK_FALSE(mir::ReadParticleEffectAsset(*mir::ParseAssetBlob(wrongTypeBytes)).mEffect);
    CHECK_FALSE(mir::ReadParticleEmitterAsset(*mir::ParseAssetBlob(wrongTypeBytes)).mEmitter);
    const auto wrongEmitterVersion = mir::BuildAssetBlob(mir::kParticleEmitterTypeTag, 999, {});
    CHECK_FALSE(mir::ReadParticleEmitterAsset(*mir::ParseAssetBlob(wrongEmitterVersion)).mEmitter);
    mir::test::ParticleTestAssets files;
    auto corrupt = mir::BuildParticleEffectAsset(files.Definition());
    corrupt.back() ^= std::byte{ 1 };
    CHECK_FALSE(mir::ParseAssetBlob(corrupt));
}

TEST_CASE("Particle motion inputs and wind survive source and cooked roundtrips", "[particles]")
{
    const auto source = mir::ReadParticleEmitterSource(
        R"({"version":3,"target":"gpu","lifetime":2,"spawnRate":30,"capacity":256,"texture":"x.png",
            "localOffset":[-2,0.5,1],"spawnPerMeter":8.5,"burst":12,"seed":4294967295,
            "inheritVelocity":0.75,"teleportDistance":150})", "assets");
    REQUIRE(source.mEmitter);
    const auto bytes = mir::BuildParticleEmitterAsset(*source.mEmitter);
    const auto cooked = mir::ReadParticleEmitterAsset(*mir::ParseAssetBlob(bytes));
    REQUIRE(cooked.mEmitter);
    CHECK(cooked.mEmitter->localOffset == glm::vec3(-2, 0.5f, 1));
    CHECK(cooked.mEmitter->spawnPerMeter == 8.5f);
    CHECK(cooked.mEmitter->burst == 12);
    CHECK(cooked.mEmitter->seed == UINT32_MAX);
    CHECK(cooked.mEmitter->inheritVelocity == 0.75f);
    CHECK(cooked.mEmitter->teleportDistance == 150);

    const auto system = mir::ReadParticleEffectSource(
        R"({"version":3,"placements":[{"id":1,"emitter":"x.emitter.json","offset":[0,0,0]}],"parameters":{"wind":[3,-1,12]}})", "assets");
    REQUIRE(system.mEffect);
    const auto systemBytes = mir::BuildParticleEffectAsset(*system.mEffect);
    const auto loaded = mir::ReadParticleEffectAsset(*mir::ParseAssetBlob(systemBytes));
    REQUIRE(loaded.mEffect);
    CHECK(loaded.mEffect->parameters.wind == glm::vec3(3, -1, 12));
    CHECK(loaded.mEffect->parameters.spawnRateScale == 1);
    CHECK(loaded.mEffect->parameters.sizeScale == 1);

    auto changedMotion = *source.mEmitter;
    changedMotion.localOffset = glm::vec3(20, -3, 1);
    changedMotion.spawnPerMeter = 0;
    changedMotion.burst = 0;
    changedMotion.seed = 5;
    changedMotion.inheritVelocity = 1;
    changedMotion.teleportDistance = 10;
    CHECK(mir::GenerateParticleScript(changedMotion) == mir::GenerateParticleScript(*source.mEmitter));
}

TEST_CASE("Particle motion sources reject malformed values before cooking", "[particles]")
{
    for (const std::string_view fields : {
        R"("localOffset":[1,2])", R"("localOffset":[1,2,1e100])", R"("localOffset":[4097,0,0])",
            R"("spawnPerMeter":-1)", R"("spawnPerMeter":65537)", R"("spawnPerMeter":"1")",
            R"("burst":-1)", R"("burst":65537)", R"("burst":1.5)",
            R"("seed":-1)", R"("seed":4294967296)", R"("seed":false)",
            R"("inheritVelocity":-1)", R"("inheritVelocity":17)", R"("inheritVelocity":1e100)",
            R"("teleportDistance":0)", R"("teleportDistance":65537)", R"("teleportDistance":null)"})
    {
        const auto text = std::string(R"({"version":3,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"x.png",)") +
            std::string(fields) + "}";
        INFO(text);
        const auto result = mir::ReadParticleEmitterSource(text, "assets");
        CHECK_FALSE(result.mEmitter);
        CHECK_FALSE(result.mError.empty());
    }
    for (const std::string_view wind : {"null", "[1,2]", "[1,2,3,4]", "[1,true,3]", "[4097,0,0]", "[0,1e100,0]"})
    {
        const auto text = std::string(R"({"version":3,"placements":[{"id":1,"emitter":"x.emitter.json","offset":[0,0,0]}],"parameters":{"wind":)") +
            std::string(wind) + "}}";
        INFO(text);
        CHECK_FALSE(mir::ReadParticleEffectSource(text, "assets").mEffect);
    }
}

TEST_CASE("Particle motion behaviors validate declared stages targets and numeric bounds", "[particles]")
{
    mir::ParticleEmitterAsset valid;
    valid.texture = mir::MakeAssetId("assets/x.png");
    valid.spawnBehaviors = { { "Initialize",{} },{ "VelocityVariation",{ 1, 2, 3 } } };
    valid.updateBehaviors = { { "Age",{} },{ "Drag",{ 2 } },{ "Integrate",{} },
        { "SizeOverLife",{ 3 } },{ "FadeOverLife",{ 0 } },{ "Flipbook",{ 4, 2, 24 } } };
    std::string error;
    REQUIRE(mir::ValidateParticleEmitterAsset(valid, error));
    const auto source = mir::GenerateParticleScript(valid);
    CHECK(source.find("p.wind.xyz - a.velocityLifetime.xyz") != std::string::npos);
    CHECK(source.find("SignedNoise(randomSeed)") != std::string::npos);
    CHECK(source.find("lerp(1.0, 3.0") != std::string::npos);
    CHECK(source.find("a.sizeRotation.w = asfloat(131076u)") != std::string::npos);
    CHECK(source.find("a.identity = uint4(instance, p.range.z, birth.sequence, 1u)") != std::string::npos);
    CHECK(source.find("UpdateParticle(a, p, birth.age)") != std::string::npos);
    CHECK(source.find("a.positionAge.xyz += p.tileDelta.xyz") != std::string::npos);

    for (const mir::ParticleBehavior &behavior : std::vector<mir::ParticleBehavior>{
        { "Drag",{ -1 } },{ "Drag",{ 10001 } },{ "Drag",{} },
        { "SizeOverLife",{ -1 } },{ "SizeOverLife",{ 65 } },{ "FadeOverLife",{ -0.1f } },{ "FadeOverLife",{ 1.1f } },
        { "Flipbook",{ 0, 1, 24 } },{ "Flipbook",{ 1.5f, 2, 24 } },{ "Flipbook",{ 1, 65, 24 } },
        { "Flipbook",{ 1, 1, -1 } },{ "Flipbook",{ 1, 1, 241 } },{ "Flipbook",{ 1, 1 } },
        { "VelocityVariation",{ 1, 2, 3 } } })
    {
        auto changed = valid;
        changed.updateBehaviors = { behavior };
        INFO(behavior.name);
        CHECK_FALSE(mir::ValidateParticleEmitterAsset(changed, error));
    }
    for (const mir::ParticleBehavior &behavior : std::vector<mir::ParticleBehavior>{
        { "VelocityVariation",{ -1, 0, 0 } },{ "VelocityVariation",{ 0, 4097, 0 } },
        { "VelocityVariation",{ 0, 1 } },{ "VelocityVariation",{ 0, 1, std::numeric_limits<float>::infinity() } },
        { "Drag",{ 1 } },{ "SizeOverLife",{ 2 } },{ "FadeOverLife",{ 0 } },{ "Flipbook",{ 1, 1, 24 } } })
    {
        auto changed = valid;
        changed.spawnBehaviors = { behavior };
        INFO(behavior.name);
        CHECK_FALSE(mir::ValidateParticleEmitterAsset(changed, error));
    }
    for (const auto &behavior : valid.updateBehaviors)
    {
        auto cpu = valid;
        cpu.simulationTarget = mir::ParticleSimulationTarget::CPU;
        cpu.spawnBehaviors.clear();
        cpu.updateBehaviors = { behavior };
        CHECK_FALSE(mir::ValidateParticleEmitterAsset(cpu, error));
    }
    auto cpu = valid;
    cpu.simulationTarget = mir::ParticleSimulationTarget::CPU;
    cpu.updateBehaviors.clear();
    cpu.spawnBehaviors = { { "VelocityVariation",{ 1, 2, 3 } } };
    CHECK_FALSE(mir::ValidateParticleEmitterAsset(cpu, error));
}

TEST_CASE("Particle renderer properties roundtrip through source and cooked emitters", "[particles][rendering]")
{
    const auto source = mir::ReadParticleEmitterSource(R"({"version":3,"target":"gpu","lifetime":2,"spawnRate":60,
        "capacity":128,"texture":"x.png","renderer":{"alignment":"velocity","facing":"position",
        "blend":"additive","sort":"none","softDistance":1.5,"maxDistance":1200,"lighting":"sunAmbient",
        "fog":false,"textured":true}})", "assets");
    REQUIRE(source.mEmitter);
    const auto bytes = mir::BuildParticleEmitterAsset(*source.mEmitter);
    const auto cooked = mir::ReadParticleEmitterAsset(*mir::ParseAssetBlob(bytes));
    REQUIRE(cooked.mEmitter);
    const auto &r = cooked.mEmitter->renderer;
    CHECK(r.alignment==1);CHECK(r.facing==1);CHECK(r.blend==1);CHECK(r.sort==1);
    CHECK(r.softDistance==1.5f);CHECK(r.maxDistance==1200);CHECK(r.lit);CHECK_FALSE(r.fog);CHECK(r.textured);
}
TEST_CASE("Particle renderer schemas reject invalid choices and nonfinite distances", "[particles][rendering]")
{
    for (const auto renderer:{ R"({"facing":"bad"})",R"({"alignment":true})",R"({"sort":0})",R"({"blend":"opaque"})",
        R"({"softDistance":-1})",R"({"maxDistance":0})",R"({"lighting":"pbr"})",R"({"fog":1})",R"({"textured":"yes"})" })
    {
        const auto source = std::string(R"({"version":3,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":128,"texture":"x.png","renderer":)")+
            renderer+"}";
        CHECK_FALSE(mir::ReadParticleEmitterSource(source, "assets").mEmitter);
    }
    mir::ParticleEmitterAsset emitter;
    emitter.texture = { 1 };
    std::string error;
    emitter.renderer.softDistance = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(mir::ValidateParticleEmitterAsset(emitter, error));
}

namespace
{
    mir::ParticleEmitterAsset EmitterWith(const mir::ParticleBehaviorInfo &info, std::vector<float> values)
    {
        mir::ParticleEmitterAsset emitter;
        emitter.texture = mir::MakeAssetId("assets/x.png");
        emitter.simulationTarget = info.target;
        const mir::ParticleBehavior behavior{ std::string(info.name), std::move(values) };
        if (info.target == mir::ParticleSimulationTarget::GPU)
        {
            // The default stacks hold the required core behaviors, the behavior under test replaces its own entry or joins them.
            auto &stack = info.spawn ? emitter.spawnBehaviors : emitter.updateBehaviors;
            const auto existing = std::find_if(stack.begin(), stack.end(),
                [&](const mir::ParticleBehavior &entry) { return entry.name == behavior.name; });
            if (existing != stack.end()) *existing = behavior;
            else stack.push_back(behavior);
            return emitter;
        }
        // CPU behaviors are only legal inside the ribbon stack, so swap the behavior under test into it.
        emitter.spawnRate = 0;
        emitter.spawnBehaviors = { { "RibbonSpawn",{ 0.25f } } };
        emitter.updateBehaviors = { { "RibbonAge",{} },{ "RibbonWind",{} },{ "RibbonWidth",{ 1 } },{ "RibbonFade",{} } };
        if (behavior.name == "RibbonSpawn") emitter.spawnBehaviors[0] = behavior;
        else if (behavior.name == "RibbonAge") emitter.updateBehaviors[0] = behavior;
        else if (behavior.name == "RibbonWind") emitter.updateBehaviors[1] = behavior;
        else if (behavior.name == "RibbonWidth" || behavior.name == "RibbonWidthCurve") emitter.updateBehaviors[2] = behavior;
        else if (behavior.name == "RibbonColorGradient") emitter.updateBehaviors.insert(emitter.updateBehaviors.begin() + 3, behavior);
        else emitter.updateBehaviors[3] = behavior;
        return emitter;
    }
}

TEST_CASE("Every registered particle behavior accepts its defaults and enforces its declared parameter ranges", "[particles]")
{
    for (const auto &info : mir::ParticleBehaviors())
    {
        INFO(info.name);
        std::vector<float> defaults;
        for (const auto &parameter : info.parameters) defaults.push_back(parameter.defaultValue);
        std::string error;
        CHECK(mir::ValidateParticleEmitterAsset(EmitterWith(info, defaults), error));
        CHECK(error.empty());
        CHECK((info.target == mir::ParticleSimulationTarget::GPU) == (info.emit != nullptr));
        for (size_t i = 0; i < defaults.size(); ++i)
        {
            const auto &range = info.parameters.begin()[i];
            INFO(range.name);
            const auto accepts = [&](float value)
                {
                    auto changed = defaults;
                    changed[i] = value;
                    std::string ignored;
                    return mir::ValidateParticleEmitterAsset(EmitterWith(info, changed), ignored);
                };
            const float inf = std::numeric_limits<float>::infinity();
            CHECK(accepts(range.min));
            CHECK(accepts(range.max));
            CHECK_FALSE(accepts(std::nextafter(range.min, -inf)));
            CHECK_FALSE(accepts(std::nextafter(range.max, inf)));
            if (range.integer) CHECK_FALSE(accepts(range.min + 0.5f));
        }
        CHECK_FALSE(mir::ValidateParticleEmitterAsset(EmitterWith(info, std::vector<float>(defaults.size() + 1, 0.5f)), error));
    }
}

TEST_CASE("New particle authoring keeps disabled values and cooks one fixed behavior order", "[particles][authoring]")
{
    mir::ParticleEmitterAsset emitter;
    emitter.texture = mir::MakeAssetId("assets/x.png");
    emitter.updateBehaviors = {{"FadeOverLife", {0}, false}, {"Integrate", {}},
        {"ColorGradient", {1, 0, 0, 1, 0, 0, 1, 0.5f}}, {"Age", {}}, {"Acceleration", {0, 1, 0}}};
    const auto source = mir::WriteParticleEmitterSource(emitter, "x.png");
    CHECK(source.find("\"version\": 3") != std::string::npos);
    const auto read = mir::ReadParticleEmitterSource(source, "assets");
    REQUIRE(read.mEmitter);
    REQUIRE(read.mEmitter->updateBehaviors.size() == 5);
    CHECK_FALSE(read.mEmitter->updateBehaviors[0].enabled);
    CHECK(read.mEmitter->updateBehaviors[0].parameters == std::vector<float>{0});
    const auto cooked = mir::ReadParticleEmitterAsset(*mir::ParseAssetBlob(mir::BuildParticleEmitterAsset(*read.mEmitter)));
    REQUIRE(cooked.mEmitter);
    REQUIRE(cooked.mEmitter->updateBehaviors.size() == 4);
    CHECK(cooked.mEmitter->updateBehaviors[0].name == "Age");
    CHECK(cooked.mEmitter->updateBehaviors[1].name == "Acceleration");
    CHECK(cooked.mEmitter->updateBehaviors[2].name == "Integrate");
    CHECK(cooked.mEmitter->updateBehaviors[3].name == "ColorGradient");
    const auto script = mir::GenerateParticleScript(*read.mEmitter);
    CHECK(script.find("a.positionAge.w +=") < script.find("a.velocityLifetime.xyz +="));
    CHECK(script.find("a.velocityLifetime.xyz +=") < script.find("a.positionAge.xyz +="));
    CHECK(script.find("a.color.a *=") == std::string::npos);
    emitter.updateBehaviors[0].enabled = true;
    const auto combined = mir::GenerateParticleScript(emitter);
    CHECK(combined.find("a.color = p.color * lerp(") < combined.find("a.color.a *= lerp(1.0"));
    emitter.updateBehaviors[2].enabled = false;
    const auto opacityOnly = mir::GenerateParticleScript(emitter);
    CHECK(opacityOnly.find("a.color.a = p.color.a * lerp(1.0") != std::string::npos);
}

TEST_CASE("Particle authoring rejects duplicate or conflicting modes", "[particles][authoring]")
{
    mir::ParticleEmitterAsset emitter;
    emitter.texture = mir::MakeAssetId("assets/x.png");
    std::string error;
    emitter.updateBehaviors.push_back({"Age", {}});
    CHECK_FALSE(mir::ValidateParticleEmitterAsset(emitter, error));
    CHECK(error.find("duplicate") != std::string::npos);
    emitter.updateBehaviors.pop_back();
    emitter.updateBehaviors.push_back({"SizeOverLife", {1}});
    emitter.updateBehaviors.push_back({"SizeCurve", {1, 2, 1}});
    CHECK_FALSE(mir::ValidateParticleEmitterAsset(emitter, error));
    CHECK(error.find("conflict") != std::string::npos);
    emitter.updateBehaviors.back().enabled = false;
    CHECK(mir::ValidateParticleEmitterAsset(emitter, error));
}

TEST_CASE("Two placements reuse one emitter asset at distinct world origins", "[particles][placement]")
{
    mir::ParticleEmitterAsset emitter;
    emitter.texture = mir::MakeAssetId("assets/x.png");
    emitter.localOffset = {0, 1, 0};
    mir::ParticleEffect effect;
    effect.emitters = {{1}, {1}};
    effect.placementIds = {10, 20};
    effect.placementOffsets = {{-2, 0, 0}, {2, 0, 0}};
    effect.emitterDefinitions = {emitter, emitter};
    const std::vector<std::string> paths{"x.emitter.json", "x.emitter.json"};
    const auto source = mir::WriteParticleEffectSource(effect, paths);
    const auto parsed = mir::ReadParticleEffectSource(source, "assets");
    REQUIRE(parsed.mEffect);
    CHECK(parsed.mEmitterPaths == paths);
    const auto cooked = mir::ReadParticleEffectAsset(*mir::ParseAssetBlob(mir::BuildParticleEffectAsset(*parsed.mEffect)));
    REQUIRE(cooked.mEffect);
    CHECK(cooked.mEffect->placementIds == std::vector<uint32_t>{10, 20});
    CHECK(cooked.mEffect->placementOffsets == effect.placementOffsets);
    mir::ParticleRuntime runtime;
    mir::WorldPose pose;
    pose.position = *mir::WorldPosition::FromMeters(mir::WorldVec3(100, 0, 0));
    const auto handle = runtime.Start({2}, effect, pose);
    REQUIRE_FALSE(handle.IsNull());
    const auto *instance = runtime.Find(handle);
    REQUIRE(instance);
    REQUIRE(instance->emitters.size() == 2);
    const auto *left = runtime.FindEmitter(instance->emitters[0]);
    const auto *right = runtime.FindEmitter(instance->emitters[1]);
    REQUIRE(left);
    REQUIRE(right);
    CHECK(left->attachmentPosition.Meters().x == 98);
    CHECK(right->attachmentPosition.Meters().x == 102);
    CHECK(left->attachmentPosition.Meters().y == 1);
    CHECK(right->attachmentPosition.Meters().y == 1);
}
