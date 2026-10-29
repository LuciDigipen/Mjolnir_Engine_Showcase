/**
 * @file PauseMenu.h
 * @author Quy Dong Ta
 * @brief The menu a game pauses into: Resume, Restart, Audio and Quit over a
 *        dimmed screen, and the Audio screen behind it, driven by gamepad,
 *        mouse or keyboard, built on gameui's public API alone.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "AudioOptions.h"

#include <gameui/UIContext.h>

#include <cstdint>

namespace mir
{

    enum class PauseMenuAction : uint8_t
    {
        None,
        Opened,
        Resumed,
        Restart,
        Quit,
    };

    class PauseMenu
    {
    public:

        PauseMenuAction Draw(UIContext &ui, bool canOpen);

        void Open() { mOpenRequested = true; }

        bool IsOpen() const { return mOpen; }

        // null: no Audio entry
        void SetAudioOptions(IAudioOptions *audio) { mAudio = audio; }

        bool AudioScreenOpen() const { return mOpen && mScreen == Screen::Audio; }

    private:
        enum class Screen : uint8_t
        {
            Main,
            Audio,
        };

        PauseMenuAction DrawMain(UIContext &ui);
        void DrawAudio(UIContext &ui);

        IAudioOptions *mAudio = nullptr;
        Screen mScreen = Screen::Main;
        bool mOpen = false;
        bool mOpenRequested = false;
    };
} // namespace mir
