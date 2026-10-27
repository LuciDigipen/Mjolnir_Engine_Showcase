/**
 * @file AudioModule.cpp
 * @author sumin.park
 * @brief Wwise, audio components and the systems that drive them.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <audio/AudioModule.h>

#include <audio/AudioEmitterSystem.h>
#include <audio/AudioListenerSystem.h>
#include <audio/AudioSystem.h>
#include <audio/ComponentRegistration.h>
#include <audio/SoundtrackSystem.h>
#include <audio/components/AudioEmitter.h>
#include <audio/components/Soundtrack.h>

#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>

namespace mir
{
    namespace
    {
        // Renders the frame's audio. Ungated: Edit and Pause still play UI sounds and fades
        class AudioRenderSystem final : public ISystem
        {
        public:
            void OnUpdate(SystemContext &) override { audio::Update(); }
        };
    }

    AudioModule::AudioModule(ModuleContext context, const std::filesystem::path &bankDir)
    {
        audio::Init(bankDir);
        RegisterAudioComponents();

        // WorldTransform readers; the render after both, so it sends this frame's positions
        context.scheduler.AddSystem<AudioEmitterSystem>(SystemPhase::LateUpdate, SystemOrder{SystemOrder::kNormal});
        context.scheduler.AddSystem<AudioListenerSystem>(SystemPhase::LateUpdate, SystemOrder{SystemOrder::kNormal});
        context.scheduler.AddSystem<SoundtrackSystem>(SystemPhase::LateUpdate, SystemOrder{SystemOrder::kNormal});
        context.scheduler.AddSystem<AudioRenderSystem>(SystemPhase::LateUpdate, SystemOrder{SystemOrder::kLate});

        mHooks = context.world.SetComponentHooks<AudioEmitter>({ .onRemove = &AudioEmitterSystem::OnEmitterRemoved });
        mSoundtrackHooks =
            context.world.SetComponentHooks<Soundtrack>({ .onRemove = &SoundtrackSystem::OnSoundtrackRemoved });
        mPlayEnd = context.scheduler.AddStage(Stage::PlayEnd, &SoundtrackSystem::OnPlayEnd);
    }

    AudioModule::~AudioModule()
    {
        mPlayEnd.Reset();
        mSoundtrackHooks.Reset();
        mHooks.Reset();
        audio::Terminate();
    }
}
