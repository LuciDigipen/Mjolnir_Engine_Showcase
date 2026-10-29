/**
 * @file EditorAudioOptions.cpp
 * @author Quy Dong Ta
 * @brief The editor's audio controls and soundtrack picker, through the audio module's public API.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <app/EditorAudioOptions.h>

#include <audio/AudioSettings.h>
#include <audio/BankEvents.h>

namespace mir
{
    namespace
    {
        EditorAudioLevels ToEditor(const AudioSettings &settings)
        {
            return {{settings.mMusic.mVolume, settings.mMusic.mMuted}, {settings.mSfx.mVolume, settings.mSfx.mMuted}};
        }

        AudioSettings ToAudio(const EditorAudioLevels &levels)
        {
            return {{levels.mMusic.mVolume, levels.mMusic.mMuted}, {levels.mSfx.mVolume, levels.mSfx.mMuted}};
        }
    }

    EditorAudioLevels EditorAudioOptions::Live() const
    {
        return ToEditor(CurrentAudioSettings());
    }

    void EditorAudioOptions::SetLive(const EditorAudioLevels &levels)
    {
        ApplyAudioSettings(ToAudio(levels));
    }

    EditorAudioLevels EditorAudioOptions::Defaults() const
    {
        return ToEditor(mAudio.Defaults());
    }

    bool EditorAudioOptions::SaveDefaults(const EditorAudioLevels &defaults)
    {
        return mAudio.SaveDefaults(ToAudio(defaults));
    }

    std::vector<EditorSoundBank> EditorAudioOptions::SoundBanks() const
    {
        std::vector<EditorSoundBank> banks;
        for (BankEvents &bank : ListBankEvents(mBankDir))
            banks.push_back({std::move(bank.mBank), std::move(bank.mEvents), bank.mListed});
        return banks;
    }
}
