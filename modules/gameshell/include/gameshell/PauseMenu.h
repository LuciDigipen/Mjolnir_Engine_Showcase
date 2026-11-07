/**
 * @file PauseMenu.h
 * @author Quy Dong Ta
 * @brief The menu a game pauses into: Resume, Restart, Audio, Controls and Quit
 *        over a dimmed screen, and the screens behind it, driven by gamepad,
 *        mouse or keyboard, built on gameui's public API alone.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "AudioOptions.h"
#include "ControlsOptions.h"

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

        // shut without a Resumed action, as leaving Play does
        void Close()
        {
            mOpen = false;
            mOpenRequested = false;
        }

        bool IsOpen() const { return mOpen; }

        // null: no Audio entry
        void SetAudioOptions(IAudioOptions *audio) { mAudio = audio; }

        bool AudioScreenOpen() const { return mOpen && mScreen == Screen::Audio; }

        // null: no Controls entry
        void SetControlsOptions(IControlsOptions *controls) { mControls = controls; }

        bool ControlsScreenOpen() const { return mOpen && mScreen == Screen::Controls; }

    private:
        enum class Screen : uint8_t
        {
            Main,
            Audio,
            Controls,
        };

        PauseMenuAction DrawMain(UIContext &ui);
        void DrawAudio(UIContext &ui);
        void DrawControls(UIContext &ui);
        void BackToMain(UIContext &ui);

        IAudioOptions *mAudio = nullptr;
        IControlsOptions *mControls = nullptr;
        Screen mScreen = Screen::Main;
        bool mOpen = false;
        bool mOpenRequested = false;
    };
} // namespace mir
