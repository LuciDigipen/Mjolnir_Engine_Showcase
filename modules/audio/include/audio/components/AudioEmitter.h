/**
 * @file AudioEmitter.h
 * @author Quy Dong Ta
 * @brief
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include <core/ecs/ComponentAsserts.h>
#include <core/ecs/ComponentFields.h>
#include <cstddef>
#include <cstdint>


namespace mir
{
    struct AudioEmitter
    {
        char mEventName[kFieldStringCapacity] = {};
        bool mPlayOnStart = false;

        bool mLoop = false;
        float mVolume = 1.0f; // 0 to 1, on top of the SFX channel

        uint64_t mGameObject = 0;
        uint32_t mWorldVersion = 0; //last world transform version pushed
        uint32_t mPlayingId = 0;    //0 = nothing playing; drives the repost and the stop
        bool mStarted = false;
        float mAppliedVolume = -1.0f; //last volume pushed; -1 = none yet
    };

    MIR_ASSERT_COMPONENT(AudioEmitter);

    inline constexpr FieldDesc kAudioEmitterFields[] = {
        // picked from the SoundBanks; no bank, as an emitter waits for every bank
        {.mName = "eventName", .mKind = FieldKind::String, .mOffset = offsetof(AudioEmitter, mEventName),
         .mWidget = FieldWidget::AssetRef, .mAssetKind = AssetKind::SoundEvent},
        {.mName = "playOnStart", .mKind = FieldKind::Bool, .mOffset = offsetof(AudioEmitter, mPlayOnStart)},
        {.mName = "loop", .mKind = FieldKind::Bool, .mOffset = offsetof(AudioEmitter, mLoop)},
        {.mName = "volume", .mKind = FieldKind::Float, .mOffset = offsetof(AudioEmitter, mVolume), .mClamped = true,
         .mMin = 0.0f, .mMax = 1.0f},
    };
    
}
