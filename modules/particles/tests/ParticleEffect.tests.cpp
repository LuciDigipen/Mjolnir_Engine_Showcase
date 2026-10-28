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

#include <limits>

TEST_CASE("Particle source cooks to a versioned definition with a stable texture dependency", "[particles]")
{
    const auto source = mir::ReadParticleEffectSource(
        R"({"version":1,"lifetime":2,"spawnRate":60,"capacity":256,"texture":"textures/white.png"})", "assets");
    REQUIRE(source.mEffect);
    CHECK(source.mEffect->texture == mir::test::ParticleTestAssets::TextureId());
    CHECK(source.mTexturePath == "textures/white.png");
    const auto bytes = mir::BuildParticleEffectAsset(*source.mEffect);
    const auto blob = mir::ParseAssetBlob(bytes);
    REQUIRE(blob);
    const auto cooked = mir::ReadParticleEffectAsset(*blob);
    REQUIRE(cooked.mEffect);
    CHECK(cooked.mEffect->lifetime == 2.0f);
    CHECK(cooked.mEffect->spawnRate == 60.0f);
    CHECK(cooked.mEffect->capacity == 256);
    CHECK(cooked.mEffect->texture == source.mEffect->texture);
}

TEST_CASE("Particle source rejects unsupported schemas unsafe paths and invalid numbers", "[particles]")
{
    for (const std::string_view source : {
        "[]", "{bad", "{}",
        R"({"version":2,"lifetime":2,"spawnRate":60,"capacity":256,"texture":"textures/white.png"})",
        R"({"version":1,"lifetime":-1,"spawnRate":60,"capacity":256,"texture":"textures/white.png"})",
        R"({"version":1,"lifetime":121,"spawnRate":60,"capacity":256,"texture":"textures/white.png"})",
        R"({"version":1,"lifetime":1e100,"spawnRate":60,"capacity":256,"texture":"textures/white.png"})",
        R"({"version":1,"lifetime":2,"spawnRate":-1,"capacity":256,"texture":"textures/white.png"})",
        R"({"version":1,"lifetime":2,"spawnRate":60,"capacity":1.5,"texture":"textures/white.png"})",
        R"({"version":1,"lifetime":2,"spawnRate":60,"capacity":65537,"texture":"textures/white.png"})",
        R"({"version":1,"lifetime":2,"spawnRate":60,"capacity":256,"texture":"../outside.png"})",
        R"({"version":1,"lifetime":2,"spawnRate":60,"capacity":256,"texture":"C:/outside.png"})",
        R"({"version":1,"lifetime":2,"spawnRate":60,"capacity":256,"texture":"x.png","typo":1})"})
    {
        const auto result = mir::ReadParticleEffectSource(source, "assets");
        CHECK_FALSE(result.mEffect);
        CHECK_FALSE(result.mError.empty());
    }
}

TEST_CASE("Particle loading validates cooked type version and texture dependency", "[particles]")
{
    mir::test::ParticleTestAssets files;
    auto manifest = mir::AssetManifest::LoadFile(files.Root() / mir::kAssetManifestFileName);
    REQUIRE(manifest);
    mir::AssetCache valid(&*manifest, files.Root());
    REQUIRE(mir::LoadParticleEffect(valid, files.EffectId()).mEffect);

    files.Publish(0.05f, false);
    mir::AssetCache missing(&*manifest, files.Root());
    CHECK_FALSE(mir::LoadParticleEffect(missing, files.EffectId()).mEffect);
    files.Publish(0.05f, true, false);
    manifest = mir::AssetManifest::LoadFile(files.Root() / mir::kAssetManifestFileName);
    mir::AssetCache wrongTexture(&*manifest, files.Root());
    CHECK_FALSE(mir::LoadParticleEffect(wrongTexture, files.EffectId()).mEffect);
    files.Publish(0.05f, true, true, 2);
    manifest = mir::AssetManifest::LoadFile(files.Root() / mir::kAssetManifestFileName);
    mir::AssetCache wrongVersion(&*manifest, files.Root());
    CHECK_FALSE(mir::LoadParticleEffect(wrongVersion, files.EffectId()).mEffect);

    const std::string bad = R"({"lifetime":1,"spawnRate":1,"capacity":1,"textureId":-1})";
    const auto bytes = mir::BuildAssetBlob(mir::kParticleEffectTypeTag, 1, std::as_bytes(std::span(bad)));
    CHECK_FALSE(mir::ReadParticleEffectAsset(*mir::ParseAssetBlob(bytes)).mEffect);

    const std::string malformed = "{broken";
    const auto malformedBytes = mir::BuildAssetBlob(mir::kParticleEffectTypeTag, 1,
        std::as_bytes(std::span(malformed)));
    CHECK_FALSE(mir::ReadParticleEffectAsset(*mir::ParseAssetBlob(malformedBytes)).mEffect);
    const auto wrongTypeBytes = mir::BuildAssetBlob(0, 1, std::as_bytes(std::span(bad)));
    CHECK_FALSE(mir::ReadParticleEffectAsset(*mir::ParseAssetBlob(wrongTypeBytes)).mEffect);
    auto corrupt = mir::BuildParticleEffectAsset({1, 1, 1, files.TextureId()});
    corrupt.back() ^= std::byte{1};
    CHECK_FALSE(mir::ParseAssetBlob(corrupt));
}
