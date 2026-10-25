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
#include <audio/components/AudioEmitter.h>

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
        context.scheduler.AddSystem<AudioEmitterSystem>(SystemPhase::LateUpdate);
        context.scheduler.AddSystem<AudioListenerSystem>(SystemPhase::LateUpdate);
        context.scheduler.AddSystem<AudioRenderSystem>(SystemPhase::LateUpdate);

        mHooks = context.world.SetComponentHooks<AudioEmitter>({ .onRemove = &AudioEmitterSystem::OnEmitterRemoved });
    }

    AudioModule::~AudioModule()
    {
        mHooks.Reset();
        audio::Terminate();
    }
}
