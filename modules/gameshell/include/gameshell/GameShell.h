/**
 * @file GameShell.h
 * @author Quy Dong Ta
 * @brief The screens every game gets: the HUD while it plays and the pause
 *        menu over it, with what the player asked of the game this frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "GameHud.h"
#include "PauseMenu.h"

#include <gameui/UIContext.h>

#include <cstdint>

namespace mir
{
    enum class GameShellAction : uint8_t
    {
        None,
        Pause,
        Resume,
        Restart,
        Quit,
    };

    class GameShell
    {
    public:

        GameShellAction Draw(UIContext &ui, bool playing, float dt);

        // Play began or ended: the menu shuts and the clock starts over, whether or not Draw ran in between
        void Reset();

        bool MenuOpen() const { return mMenu.IsOpen(); }

        bool AudioScreenOpen() const { return mMenu.AudioScreenOpen(); }

        // null = the menu has no Audio entry
        void SetAudioOptions(IAudioOptions *audio) { mMenu.SetAudioOptions(audio); }

        bool ControlsScreenOpen() const { return mMenu.ControlsScreenOpen(); }

        // null = the menu has no Controls entry
        void SetControlsOptions(IControlsOptions *controls) { mMenu.SetControlsOptions(controls); }

        float PlaySeconds() const { return mPlaySeconds; }

        // the mouse goes unused this long in play before its cursor is hidden
        static constexpr float kCursorIdleSeconds = 2.0f;

    private:
        void UpdateCursor(UIContext &ui, float dt);

        GameHud mHud;
        PauseMenu mMenu;
        float mPlaySeconds = 0.0f;
        float mCursorIdleSeconds = 0.0f;
        bool mWasPlaying = false;
    };
} // namespace mir
