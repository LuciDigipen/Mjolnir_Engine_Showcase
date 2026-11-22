/**
 * @file GameShell.cpp
 * @author Quy Dong Ta
 * @brief The screens every game gets: the HUD while it plays and the pause
 *        menu over it, with what the player asked of the game this frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "gameshell/GameShell.h"

#include <input/DefaultInputMap.h>

namespace mir
{
    GameShellAction GameShell::Draw(UIContext &ui, bool playing, float dt)
    {
        if (!playing)
        {
            mWasPlaying = false;
            mMenu.Draw(ui, false);
            return GameShellAction::None;
        }

        if (!mWasPlaying)
            Reset();
        mWasPlaying = true;
        if (!mMenu.IsOpen())
            mPlaySeconds += dt;

        if (mParts.mHud && mHud.Draw(ui, mPlaySeconds, !mMenu.IsOpen(), mParts.mMenu))
            mMenu.Open();

        if (!mParts.mMenu)
            return GameShellAction::None;

        const PauseMenuAction menuAction = mMenu.Draw(ui, true);
        UpdateCursor(ui, dt);

        switch (menuAction)
        {
        case PauseMenuAction::Opened:
            return GameShellAction::Pause;
        case PauseMenuAction::Resumed:
            return GameShellAction::Resume;
        case PauseMenuAction::Restart:
            mPlaySeconds = 0.0f;
            return GameShellAction::Restart;
        case PauseMenuAction::Quit:
            return GameShellAction::Quit;
        case PauseMenuAction::None:
            break;
        }
        return GameShellAction::None;
    }

    void GameShell::Reset()
    {
        mMenu.Close();
        mPlaySeconds = 0.0f;
        mCursorIdleSeconds = kCursorIdleSeconds;
        mWasPlaying = false;
    }

    // Shown in the menu, and in play while the mouse is moving, so the HUD's Pause button can be found;
    // hidden while it rests, while another device is in use, and while it looks the camera around
    void GameShell::UpdateCursor(UIContext &ui, float dt)
    {
        if (mMenu.IsOpen())
        {
            mCursorIdleSeconds = 0.0f;
            return;
        }

        const UIState &state = ui.State();
        mCursorIdleSeconds = state.PointerMoved() ? 0.0f : mCursorIdleSeconds + dt;

        // in the editor's viewport, a cursor over the panels around it is the editor's
        if (!Contains(ui.Bounds(), state.PointerX(), state.PointerY()))
            return;

        const InputState *input = ui.Input();
        const bool mouseInUse = input != nullptr && input->ActiveDevice() == DeviceKind::Mouse;
        const bool looking = input != nullptr && input->ForGame(kCameraLookHoldAction).down;
        if (!mouseInUse || looking || mCursorIdleSeconds >= kCursorIdleSeconds)
            ui.HideCursor();
    }
} // namespace mir
