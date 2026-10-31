/**
 * @file AudioComponentFields.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for the audio components' fields as the editor's inspector shows them.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <audio/components/AudioEmitter.h>
#include <audio/components/Soundtrack.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <iterator>
#include <span>
#include <string_view>

namespace
{
    const mir::FieldDesc *Find(std::span<const mir::FieldDesc> fields, std::string_view name)
    {
        const auto it = std::find_if(fields.begin(), fields.end(),
            [&](const mir::FieldDesc &field) { return field.mName == name; });
        return it != fields.end() ? &*it : nullptr;
    }
}

TEST_CASE("The soundtrack's event is picked from the SoundBanks, and the pick sets its hidden bank", "[audio]")
{
    const mir::FieldDesc *event = Find(mir::kSoundtrackFields, "eventName");
    REQUIRE(event != nullptr);
    CHECK(event->mWidget == mir::FieldWidget::AssetRef);
    CHECK(event->mAssetKind == mir::AssetKind::SoundEvent);

    const mir::FieldDesc *bank = Find(mir::kSoundtrackFields, event->mBankField);
    REQUIRE(bank != nullptr);
    CHECK(bank->mName == "bankName");
    CHECK(bank->mKind == mir::FieldKind::String);
    CHECK(bank->mHidden);
}

TEST_CASE("An emitter's event is picked from the SoundBanks, with no bank to set", "[audio]")
{
    const mir::FieldDesc *event = Find(mir::kAudioEmitterFields, "eventName");
    REQUIRE(event != nullptr);
    CHECK(event->mWidget == mir::FieldWidget::AssetRef);
    CHECK(event->mAssetKind == mir::AssetKind::SoundEvent);
    CHECK(event->mBankField.empty());
}

TEST_CASE("An emitter and a soundtrack each have a volume of their own, from 0 to 1 and full by default", "[audio]")
{
    for (const std::span<const mir::FieldDesc> fields :
         {std::span<const mir::FieldDesc>(mir::kAudioEmitterFields), std::span<const mir::FieldDesc>(mir::kSoundtrackFields)})
    {
        const mir::FieldDesc *volume = Find(fields, "volume");
        REQUIRE(volume != nullptr);
        CHECK(volume->mKind == mir::FieldKind::Float);
        CHECK(volume->mClamped);
        CHECK(volume->mMin == 0.0f);
        CHECK(volume->mMax == 1.0f);
    }

    CHECK(mir::AudioEmitter{}.mVolume == 1.0f);
    CHECK(mir::Soundtrack{}.mVolume == 1.0f);
}
