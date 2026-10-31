/**
 * @file GameAudioOptions.cpp
 * @author Quy Dong Ta
 * @brief The pause menu's Audio screen, through the audio module's public API.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <app/GameAudioOptions.h>

#include <audio/AudioSettings.h>
#include <audio/AudioSystem.h>

namespace mir
{
    float GameAudioOptions::Volume(AudioChannel channel) const
    {
        return channel == AudioChannel::Music ? audio::GetMusicVolume() : audio::GetSfxVolume();
    }

    void GameAudioOptions::SetVolume(AudioChannel channel, float volume)
    {
        if (channel == AudioChannel::Music)
            audio::SetMusicVolume(volume);
        else
            audio::SetSfxVolume(volume);
    }

    bool GameAudioOptions::Muted(AudioChannel channel) const
    {
        return channel == AudioChannel::Music ? audio::IsMusicMuted() : audio::IsSfxMuted();
    }

    void GameAudioOptions::SetMuted(AudioChannel channel, bool muted)
    {
        if (channel == AudioChannel::Music)
            audio::SetMusicMuted(muted);
        else
            audio::SetSfxMuted(muted);
    }

    void GameAudioOptions::RestoreDefaults()
    {
        ApplyAudioSettings(mAudio.Defaults());
    }
}
