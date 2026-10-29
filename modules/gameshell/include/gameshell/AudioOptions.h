/**
 * @file AudioOptions.h
 * @author Quy Dong Ta
 * @brief What the pause menu's Audio screen reads and changes: the music and SFX
 *        volume and mute, and the project's defaults. An interface, so gameshell
 *        builds without the audio module.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>

namespace mir
{
    enum class AudioChannel : uint8_t
    {
        Music,
        Sfx,
    };

    class IAudioOptions
    {
    public:
        virtual ~IAudioOptions() = default;

        // 0 to 1
        virtual float Volume(AudioChannel channel) const = 0;
        virtual void SetVolume(AudioChannel channel, float volume) = 0;

        virtual bool Muted(AudioChannel channel) const = 0;
        virtual void SetMuted(AudioChannel channel, bool muted) = 0;

        // Both channels back to the project's defaults
        virtual void RestoreDefaults() = 0;
    };
} // namespace mir
