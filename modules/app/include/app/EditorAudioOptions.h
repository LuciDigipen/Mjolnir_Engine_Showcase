/**
 * @file EditorAudioOptions.h
 * @author Quy Dong Ta
 * @brief The editor's audio controls and soundtrack picker, through the audio module's public API.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <audio/AudioModule.h>
#include <editor/EditorAudio.h>

#include <filesystem>
#include <utility>

namespace mir
{
    class EditorAudioOptions final : public IEditorAudio
    {
    public:
        EditorAudioOptions(AudioModule &audio, std::filesystem::path bankDir)
            : mAudio(audio), mBankDir(std::move(bankDir))
        {
        }

        EditorAudioLevels Live() const override;
        void SetLive(const EditorAudioLevels &levels) override;

        EditorAudioLevels Defaults() const override;
        bool SaveDefaults(const EditorAudioLevels &defaults) override;
        void SetDefaults(const EditorAudioLevels &defaults) override;
        bool ReloadDefaults(std::string &problem) override;

        std::vector<EditorSoundBank> SoundBanks() const override;

    private:
        AudioModule &mAudio;
        std::filesystem::path mBankDir;
    };
}
