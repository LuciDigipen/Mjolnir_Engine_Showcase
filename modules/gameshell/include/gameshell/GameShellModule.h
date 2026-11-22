/**
 * @file GameShellModule.h
 * @author sumin.park
 * @brief Registers the HUD and pause menu, or the widget demo, as the game UI's screen.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <gameshell/GameShell.h>

#include <gameui/UIDemoScreen.h>

#include <core/ecs/ModuleContext.h>
#include <core/Registration.h>

namespace mir
{
    class AssetResolver;
    class AssetsModule;
    class GameUIModule;

    struct GameShellSettings
    {
        // debug: start with every core widget instead of the menus (--ui-demo); GameUIDemo switches it later
        bool mUIDemo = false;
        // game.json's "shell", a game drawing its own pause menu turns the shell's off
        GameShellParts mParts;
    };

    class GameShellModule
    {
    public:
        GameShellModule(ModuleContext context, GameUIModule &gameUI, AssetsModule &assets,
            const AssetResolver &resolver, const GameShellSettings &settings);

        GameShellModule(const GameShellModule &) = delete;
        GameShellModule &operator=(const GameShellModule &) = delete;

        // The pause menu's Audio screen; set once the audio is built, null leaves it out
        void SetAudioOptions(IAudioOptions *audio) { mShell.SetAudioOptions(audio); }

        // The pause menu's Controls screen; null leaves it out
        void SetControlsOptions(IControlsOptions *controls) { mShell.SetControlsOptions(controls); }

    private:
        void Draw(UIContext &ui, float dt);

        // what the player asked of the game, as requests App applies next frame
        void Apply(GameShellAction action);

        World &mWorld;
        AssetsModule &mAssets;
        const AssetResolver &mResolver;
        GameShellSettings mSettings;
        GameShell mShell;
        UIDemoScreen mDemo;
        bool mDemoLoaded = false;
        bool mDemoShown = false;
        // the demo swaps the style's sprites; restored when the menus come back
        UIStyle mShellStyle;

        // last: unregister before the shell and screens above are destroyed
        Registration mPlayBegin;
        Registration mPlayEnd;
        Registration mScreen;
    };
}
