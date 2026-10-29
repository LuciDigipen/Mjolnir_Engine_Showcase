/**
 * @file Soundtrack.h
 * @author Quy Dong Ta
 * @brief The music a scene plays from Play to Stop: a Wwise event and the bank it lives in.
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
    struct Soundtrack
    {
        char mEventName[kFieldStringCapacity] = {};
        char mBankName[kFieldStringCapacity] = {}; // empty: wait for every bank instead
        float mVolume = 1.0f; // 0 to 1, on top of the Music channel
        int32_t mFadeOutMs = 1000;

        bool mStarted = false;
        float mAppliedVolume = -1.0f; // last volume pushed; -1 = none yet
    };

    MIR_ASSERT_COMPONENT(Soundtrack);

    // the event is picked from the SoundBanks, which sets its bank too
    inline constexpr FieldDesc kSoundtrackFields[] = {
        {.mName = "eventName", .mKind = FieldKind::String, .mOffset = offsetof(Soundtrack, mEventName),
         .mWidget = FieldWidget::AssetRef, .mAssetKind = AssetKind::SoundEvent, .mBankField = "bankName"},
        {.mName = "bankName", .mKind = FieldKind::String, .mOffset = offsetof(Soundtrack, mBankName),
         .mHidden = true},
        {.mName = "volume", .mKind = FieldKind::Float, .mOffset = offsetof(Soundtrack, mVolume), .mClamped = true,
         .mMin = 0.0f, .mMax = 1.0f},
        {.mName = "fadeOutMs", .mKind = FieldKind::Int, .mOffset = offsetof(Soundtrack, mFadeOutMs)},
    };
}
