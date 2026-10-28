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

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <limits>
#include <tuple>
#include <utility>

TEST_CASE("Particle system source cooks emitter references and user parameter defaults", "[particles]")
{
    const auto source = mir::ReadParticleEffectSource(
        R"({"version":2,"emitters":["effects/exhaust.emitter.json"],"parameters":{"spawnRateScale":2,"sizeScale":0.5}})", "assets");
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
        R"({"version":1,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"textures/white.png",
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
        R"({"version":1,"emitters":["effects/exhaust.emitter.json"]})",
        R"({"version":2,"emitters":[]})",
        R"({"version":2,"emitters":[1]})",
        R"({"version":2,"emitters":["effects/exhaust.emitter.json","effects/exhaust.emitter.json"]})",
        R"({"version":2,"emitters":["../outside.emitter.json"]})",
        R"({"version":2,"emitters":["C:/outside.emitter.json"]})",
        R"({"version":2,"emitters":["texture.png"]})",
        R"({"version":2,"emitters":["effects/exhaust.emitter.json"],"typo":1})",
        R"({"version":2,"emitters":["effects/exhaust.emitter.json"],"parameters":null})",
        R"({"version":2,"emitters":["effects/exhaust.emitter.json"],"parameters":{"spawnRateScale":-1}})",
        R"({"version":2,"emitters":["effects/exhaust.emitter.json"],"parameters":{"sizeScale":17}})",
        R"({"version":2,"emitters":["effects/exhaust.emitter.json"],"parameters":{"sizeScale":1e100}})",
        R"({"version":2,"emitters":["effects/exhaust.emitter.json"],"parameters":{"sizeScale":true}})",
        R"({"version":2,"emitters":["effects/exhaust.emitter.json"],"parameters":{"unknown":1}})"})
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
        R"({"version":2,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"textures/white.png"})",
        R"({"version":1,"target":"other","lifetime":2,"spawnRate":60,"capacity":256,"texture":"textures/white.png"})",
        R"({"version":1,"target":"gpu","lifetime":-1,"spawnRate":60,"capacity":256,"texture":"textures/white.png"})",
        R"({"version":1,"target":"gpu","lifetime":121,"spawnRate":60,"capacity":256,"texture":"textures/white.png"})",
        R"({"version":1,"target":"gpu","lifetime":1e100,"spawnRate":60,"capacity":256,"texture":"textures/white.png"})",
        R"({"version":1,"target":"gpu","lifetime":2,"spawnRate":-1,"capacity":256,"texture":"textures/white.png"})",
        R"({"version":1,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":1.5,"texture":"textures/white.png"})",
        R"({"version":1,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":65537,"texture":"textures/white.png"})",
        R"({"version":1,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"../outside.png"})",
        R"({"version":1,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"C:/outside.png"})",
        R"({"version":1,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"x.png","typo":1})",
        R"({"version":1,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"x.png","renderer":{"size":0}})",
        R"({"version":1,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"x.png","renderer":{"color":[1,1,1,2]}})",
        R"({"version":1,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"x.png","renderer":{"velocity":[0,1]}})",
        R"({"version":1,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"x.png","renderer":{"velocity":[0,1e100,0]}})",
        R"({"version":1,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"x.png","renderer":{"typo":1}})"})
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
        const uint32_t words[]{0, 0, 0, 0, 0};
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

TEST_CASE("Particle module stacks reject unknown modules unsupported stages targets and parameters", "[particles]")
{
    for (const std::string_view fields : {
        R"("spawnModules":[{"name":"Unknown"}])",
        R"("spawnModules":[{"name":"Integrate"}])",
        R"("updateModules":[{"name":"Initialize"}])",
        R"("updateModules":[{"name":"Age","parameters":[1]}])",
        R"("updateModules":[{"name":"Acceleration","parameters":[1,2]}])",
        R"("updateModules":[{"name":"Acceleration","parameters":[1,2,1e100]}])",
        R"("updateModules":[{"name":"Acceleration","parameters":[1,"2",3]}])",
        R"("updateModules":[{"name":"Age","typo":0}])",
        R"("updateModules":[{"parameters":[]}])",
        R"("updateModules":{})"})
    {
        const auto text = std::string(R"({"version":1,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"x.png",)") +
            std::string(fields) + "}";
        const auto result = mir::ReadParticleEmitterSource(text, "assets");
        INFO(text);
        CHECK_FALSE(result.mEmitter);
        CHECK_FALSE(result.mError.empty());
    }
    const auto cpu = mir::ReadParticleEmitterSource(
        R"({"version":1,"target":"cpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"x.png","updateModules":[{"name":"Age"}]})", "assets");
    CHECK_FALSE(cpu.mEmitter);
    mir::ParticleEmitterAsset emitter;
    emitter.texture = mir::MakeAssetId("assets/x.png");
    emitter.updateModules = {{"Acceleration", {0, std::numeric_limits<float>::infinity(), 0}}};
    std::string error;
    CHECK_FALSE(mir::ValidateParticleEmitterAsset(emitter, error));
}

TEST_CASE("Particle module stack order survives cooking and determines generated execution order", "[particles]")
{
    const auto parsed = mir::ReadParticleEmitterSource(
        R"({"version":1,"target":"gpu","lifetime":2,"spawnRate":60,"capacity":256,"texture":"x.png",
            "spawnModules":[{"name":"Initialize"}],
            "updateModules":[{"name":"Acceleration","parameters":[0,-9.8,0]},{"name":"Integrate"},{"name":"Age"}]})", "assets");
    REQUIRE(parsed.mEmitter);
    const auto bytes = mir::BuildParticleEmitterAsset(*parsed.mEmitter);
    const auto cooked = mir::ReadParticleEmitterAsset(*mir::ParseAssetBlob(bytes));
    REQUIRE(cooked.mEmitter);
    REQUIRE(cooked.mEmitter->updateModules.size() == 3);
    CHECK(cooked.mEmitter->updateModules[0].name == "Acceleration");
    CHECK(cooked.mEmitter->updateModules[0].parameters == (std::vector<float>{0, -9.8f, 0}));
    CHECK(cooked.mEmitter->updateModules[1].name == "Integrate");
    CHECK(cooked.mEmitter->updateModules[2].name == "Age");
    const auto source = mir::GenerateParticleScript(*cooked.mEmitter);
    const auto acceleration = source.find("a.velocityLifetime.xyz +=");
    const auto integrate = source.find("a.positionAge.xyz +=");
    const auto age = source.find("a.positionAge.w +=");
    REQUIRE(acceleration != std::string::npos);
    REQUIRE(integrate != std::string::npos);
    REQUIRE(age != std::string::npos);
    CHECK(acceleration < integrate);
    CHECK(integrate < age);
    auto reversed = *cooked.mEmitter;
    std::swap(reversed.updateModules[0], reversed.updateModules[2]);
    const auto changed = mir::GenerateParticleScript(reversed);
    CHECK(changed != source);
    CHECK(changed.find("a.positionAge.w +=") < changed.find("a.velocityLifetime.xyz +="));
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
    second.color = {0.5f, 0.5f, 0.5f, 0.5f};
    second.velocity = {1, 2, 3};
    CHECK(mir::GenerateParticleScript(first) == mir::GenerateParticleScript(second));
    second.updateModules.push_back({"Acceleration", {0, -1, 0}});
    CHECK(mir::GenerateParticleScript(first) != mir::GenerateParticleScript(second));
}

TEST_CASE("Particle blob readers reject malformed type version and asset framing", "[particles]")
{
    const std::string badSystem = R"({"emitterIds":[-1],"parameters":{}})";
    const std::string badEmitter = R"({"target":"gpu","lifetime":1,"spawnRate":1,"capacity":1,"textureId":-1})";
    const std::string malformed = "{broken";
    for (const auto &[tag, version, text] : {
        std::tuple{mir::kParticleEffectTypeTag, mir::kParticleEffectContentVersion, badSystem},
        std::tuple{mir::kParticleEffectTypeTag, mir::kParticleEffectContentVersion, malformed}})
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
    corrupt.back() ^= std::byte{1};
    CHECK_FALSE(mir::ParseAssetBlob(corrupt));
}
