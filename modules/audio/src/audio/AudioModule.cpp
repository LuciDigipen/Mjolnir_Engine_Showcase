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
#include <audio/AudioSettings.h>
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
        // Renders the frame's audio, Edit and Pause still play UI sounds and fades
        class AudioRenderSystem final : public ISystem
        {
        public:
            void OnUpdate(SystemContext &) override { audio::Update(); }
        };

        constexpr double kMinSaveIntervalSeconds = 0.5; // a slider being dragged writes on every frame
        constexpr double kRetryAfterFailureSeconds = 5.0; // each failure logs an error


        class AudioSettingsSystem final : public ISystem
        {
        public:
            AudioSettingsSystem(std::filesystem::path path, const AudioSettings &saved)
                : mPath(std::move(path)), mSaved(saved)
            {
            }

            void OnUpdate(SystemContext &context) override
            {
                if (context.elapsed - mLastSave >= kMinSaveIntervalSeconds)
                    Save(context.elapsed);
            }

            void OnStop(SystemContext &) override { Save(mLastSave); }

        private:
            void Save(double now)
            {
                const AudioSettings current = CurrentAudioSettings();
                if (current == mSaved)
                    return;

                // a failed write keeps mSaved stale, so it is retried later and again at shutdown
                if (SaveAudioSettings(mPath, current))
                {
                    mSaved = current;
                    mLastSave = now;
                }
                else
                {
                    mLastSave = now + kRetryAfterFailureSeconds - kMinSaveIntervalSeconds;
                }
            }

            std::filesystem::path mPath;
            AudioSettings mSaved;
            double mLastSave = -kMinSaveIntervalSeconds;
        };
    }

    AudioModule::AudioModule(ModuleContext context, const std::filesystem::path &bankDir,
        const std::filesystem::path &projectSettingsPath, const std::filesystem::path &settingsPath)
        : mDefaults(LoadAudioSettings(projectSettingsPath))
    {
        // before Init, which applies the volumes without a ramp
        const AudioSettings settings = LoadAudioSettings(settingsPath, mDefaults);
        ApplyAudioSettings(settings);

        audio::Init(bankDir);
        RegisterAudioComponents();

        // WorldTransform readers; the render after both, so it sends this frame's positions
        context.scheduler.AddSystem<AudioEmitterSystem>(SystemPhase::LateUpdate, SystemOrder{SystemOrder::kNormal});
        context.scheduler.AddSystem<AudioListenerSystem>(SystemPhase::LateUpdate, SystemOrder{SystemOrder::kNormal});
        context.scheduler.AddSystem<SoundtrackSystem>(SystemPhase::LateUpdate, SystemOrder{SystemOrder::kNormal});
        context.scheduler.AddSystem<AudioRenderSystem>(SystemPhase::LateUpdate, SystemOrder{SystemOrder::kLate});
        context.scheduler.AddSystem<AudioSettingsSystem>(SystemPhase::LateUpdate, SystemOrder{SystemOrder::kLate},
            settingsPath, settings);

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
