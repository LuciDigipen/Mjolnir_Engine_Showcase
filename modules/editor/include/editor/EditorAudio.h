/**
 * @file EditorAudio.h
 * @author Quy Dong Ta
 * @brief What the editor reads and changes of the game's audio: what is heard
 *        now, the project's default volumes and the events in its SoundBanks.
 *        An interface, so the editor builds without the audio module.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <string>
#include <vector>

namespace mir
{
    struct EditorAudioChannel
    {
        float mVolume = 1.0f; // 0 to 1
        bool mMuted = false;

        bool operator==(const EditorAudioChannel &) const = default;
    };

    struct EditorAudioLevels
    {
        EditorAudioChannel mMusic;
        EditorAudioChannel mSfx;

        bool operator==(const EditorAudioLevels &) const = default;
    };

    struct EditorSoundBank
    {
        std::string mName;
        std::vector<std::string> mEvents;
        bool mListed = false; // false = no <bank>.json beside the bank, so its events are unknown
    };

    class IEditorAudio
    {
    public:
        virtual ~IEditorAudio() = default;

        // What is heard now, saved to the per-user settings file like any change
        virtual EditorAudioLevels Live() const = 0;
        virtual void SetLive(const EditorAudioLevels &levels) = 0;

        // The "audio" section of project_settings.json, which a player starts from
        virtual EditorAudioLevels Defaults() const = 0;
        // Also what the pause menu's Restore defaults goes back to; false when the write fails
        virtual bool SaveDefaults(const EditorAudioLevels &defaults) = 0;
        // Changes the defaults in memory only, so a player without a settings file hears them
        virtual void SetDefaults(const EditorAudioLevels &defaults) = 0;
        // Takes the saved section back; false with the reason in problem, and nothing changes
        virtual bool ReloadDefaults(std::string &problem) = 0;

        // Read from the SoundBank folder on each call
        virtual std::vector<EditorSoundBank> SoundBanks() const = 0;
    };
}
