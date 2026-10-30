/**
 * @file GameAudioOptions.h
 * @author Quy Dong Ta
 * @brief The pause menu's Audio screen, through the audio module's public API.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <audio/AudioModule.h>
#include <gameshell/AudioOptions.h>

namespace mir
{
    class GameAudioOptions final : public IAudioOptions
    {
    public:
        explicit GameAudioOptions(const AudioModule &audio) : mAudio(audio) {}

        float Volume(AudioChannel channel) const override;
        void SetVolume(AudioChannel channel, float volume) override;

        bool Muted(AudioChannel channel) const override;
        void SetMuted(AudioChannel channel, bool muted) override;

        void RestoreDefaults() override;

    private:
        const AudioModule &mAudio;
    };
}
