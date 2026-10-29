/**
 * @file SoundtrackSystem.cpp
 * @author Quy Dong Ta
 * @brief Starts the scene's Soundtrack on Play and cuts it on Stop.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <audio/SoundtrackSystem.h>

#include <audio/AudioSystem.h>

#include <core/ecs/EngineMode.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>

#include <algorithm>
#include <cstddef>

namespace mir
{
    namespace
    {
        constexpr int kCutFadeMs = 10; // a hard stop mid-waveform clicks

        void StopSoundtrack(Soundtrack &track, int fadeMs)
        {
            if (!track.mStarted)
                return;

            audio::StopMusic(fadeMs);
            track.mStarted = false;
        }
    }

    void SoundtrackSystem::OnStart(SystemContext &context)
    {
        mQuery = &context.world.GetOrCreateQuery<Soundtrack>();
    }

    void SoundtrackSystem::OnUpdate(SystemContext &context)
    {
        if (!audio::IsInitialized())
            return;

        // the owner keeps the music whatever order the query visits them in, so only one is ever started
        Soundtrack *track = mOwner.IsNull() ? nullptr : context.world.GetComponent<Soundtrack>(mOwner);
        std::size_t count = 0;
        mQuery->ForEach([&](Entity entity, Soundtrack &candidate)
            {
                ++count;
                if (track == nullptr)
                {
                    track = &candidate;
                    mOwner = entity;
                }
            });

        if (count > 1 && !mWarnedMultipleSoundtracks)
        {
            MIR_LOG_WARN("more than one Soundtrack in the scene; only one plays");
            mWarnedMultipleSoundtracks = true;
        }

        if (track == nullptr)
            return;

        if (track->mBankName[0] != '\0')
            audio::RequestBank(track->mBankName);

        
        const bool playing = IsPlaying(context.world);
        if (playing && !track->mStarted && track->mEventName[0] != '\0')
        {
            track->mStarted = audio::PlayMusic(track->mEventName, track->mBankName);
           
            track->mAppliedVolume = -1.0f;
        }
        else if (!playing)
            StopSoundtrack(*track, kCutFadeMs);

        if (track->mStarted && track->mVolume != track->mAppliedVolume)
        {
            audio::SetSoundtrackVolume(track->mVolume);
            track->mAppliedVolume = track->mVolume;
        }
    }

    void SoundtrackSystem::OnPlayEnd(SystemContext &context)
    {
        // before the restore removes Play's entities, whose OnRemove would fade instead
        context.world.GetOrCreateQuery<Soundtrack>().ForEach(
            [](Entity, Soundtrack &track) { StopSoundtrack(track, kCutFadeMs); });
    }

    void SoundtrackSystem::OnStop(SystemContext &)
    {
        if (mQuery == nullptr)
            return;

        mQuery->ForEach([](Entity, Soundtrack &track) { track.mStarted = false; });
        audio::StopMusic(0);
    }

    void SoundtrackSystem::OnSoundtrackRemoved(World &world, Entity entity)
    {
        Soundtrack *track = world.GetComponent<Soundtrack>(entity);
        if (track != nullptr)
            StopSoundtrack(*track, std::max(0, track->mFadeOutMs));
    }
}
