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
        int32_t mFadeOutMs = 1000;

        bool mStarted = false;
    };

    MIR_ASSERT_COMPONENT(Soundtrack);

    inline constexpr FieldDesc kSoundtrackFields[] = {
        {"eventName", FieldKind::String, offsetof(Soundtrack, mEventName)},
        {"bankName", FieldKind::String, offsetof(Soundtrack, mBankName)},
        {"fadeOutMs", FieldKind::Int, offsetof(Soundtrack, mFadeOutMs)},
    };
}
