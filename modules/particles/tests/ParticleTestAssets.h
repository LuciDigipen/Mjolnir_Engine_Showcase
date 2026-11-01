/**
 * @file ParticleTestAssets.h
 * @author sumin.park
 * @brief Owns isolated cooked test assets without a graphics device.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetManifest.h>
#include <assets/ModelAsset.h>
#include <core/fs/FileIO.h>
#include <particles/ParticleEffect.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <stdexcept>

namespace mir::test
{
    class ParticleTestAssets
    {
    public:
        ParticleTestAssets()
        {
            mParent = std::filesystem::canonical(std::filesystem::current_path());
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            mRoot = mParent / ("particle-test-" + std::to_string(stamp) + "-" + std::to_string(sNext++));
            if (!std::filesystem::create_directory(mRoot))
                throw std::runtime_error("cannot create particle test directory");
            Publish();
        }
        ~ParticleTestAssets()
        {
            std::error_code error;
            if (std::filesystem::weakly_canonical(mRoot, error).parent_path() == mParent && !error)
                std::filesystem::remove_all(mRoot, error);
        }
        ParticleTestAssets(const ParticleTestAssets &) = delete;
        ParticleTestAssets &operator=(const ParticleTestAssets &) = delete;

        static AssetId EffectId() { return MakeAssetId("assets/effects/exhaust.particle.json"); }
        static AssetId EmitterId() { return MakeAssetId("assets/effects/exhaust.emitter.json"); }
        static AssetId TextureId() { return MakeAssetId("assets/textures/white.png"); }
        const std::filesystem::path &Root() const { return mRoot; }
        ParticleUserParameters authoredParameters;
        ParticleEffect Definition(float lifetime = 0.05f) const
        {
            ParticleEmitterAsset emitter;
            emitter.lifetime = lifetime;
            emitter.texture = TextureId();
            emitter.script = AssetId{ 901 };
            emitter.scriptLayout = AssetId{ 902 };
            ParticleEffect system;
            system.emitters.push_back(EmitterId());
            system.emitterDefinitions.push_back(emitter);
            system.parameters = authoredParameters;
            return system;
        }

        void Write(std::string_view name, const std::vector<std::byte> &bytes)
        {
            if (!WriteBytes(mRoot / name, bytes))
                throw std::runtime_error("cannot write particle test data");
        }

        void Publish(float lifetime = 0.05f, bool texturePresent = true, bool textureTyped = true,
            uint32_t effectVersion = kParticleEffectContentVersion)
        {
            auto effect = BuildParticleEffectAsset(Definition(lifetime));
            if (effectVersion != kParticleEffectContentVersion)
            {
                const auto view = ParseAssetBlob(effect);
                effect = BuildAssetBlob(kParticleEffectTypeTag, effectVersion, view->content);
            }
            Write("effect.blob", effect);
            Write("emitter.blob", BuildParticleEmitterAsset(Definition(lifetime).emitterDefinitions[0]));
            const auto texture = textureTyped
                ? BuildTypedAsset(TextureAsset{ 1, 1,{ 255, 255, 255, 255 } })
                : BuildAssetBlob(kRawAssetTypeTag, 1, {});
            if (texturePresent)
                Write("texture.blob", texture);
            else
            {
                std::error_code error;
                std::filesystem::remove(mRoot / "texture.blob", error);
            }
            const uint32_t code[]{ 0x07230203, 0x00010500, 0, 1, 0 };
            Write("script.blob", BuildAssetBlob(kParticleScriptTypeTag, kParticleScriptContentVersion, std::as_bytes(std::span(code))));
            const auto layout = ParticleScriptLayout();
            Write("layout.blob", BuildAssetBlob(kParticleScriptLayoutTypeTag, kParticleScriptLayoutContentVersion, std::as_bytes(std::span(layout))));
            const AssetManifestSourceEntry entries[]{
                { { AssetId{ 901 }, kParticleScriptTypeTag, kParticleScriptContentVersion }, "script.blob" },
                { { AssetId{ 902 }, kParticleScriptLayoutTypeTag, kParticleScriptLayoutContentVersion }, "layout.blob" },
                { { EffectId(), kParticleEffectTypeTag, effectVersion }, "effect.blob", "assets/effects/exhaust.particle.json" },
                { { EmitterId(), kParticleEmitterTypeTag, kParticleEmitterContentVersion }, "emitter.blob" },
                { { TextureId(), textureTyped ? kTextureAssetTypeTag : kRawAssetTypeTag,
                    textureTyped ? kTextureAssetContentVersion : kRawAssetContentVersion }, "texture.blob" } };
            Write(kAssetManifestFileName, BuildAssetManifestBlob(entries));
        }

    private:
        inline static std::atomic<uint64_t> sNext = 0;
        std::filesystem::path mParent;
        std::filesystem::path mRoot;
    };
}
