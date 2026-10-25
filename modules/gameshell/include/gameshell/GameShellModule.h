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
    class AssetsModule;
    class GameUIModule;

    struct GameShellSettings
    {
        // debug: every core widget instead of the menus (--ui-demo)
        bool mUIDemo = false;
        // the pause menu's Quit leaves Play, as in the editor, instead of closing the game
        bool mQuitStopsPlay = false;
    };

    class GameShellModule
    {
    public:
        GameShellModule(ModuleContext context, GameUIModule &gameUI, AssetsModule &assets,
            const GameShellSettings &settings);

        GameShellModule(const GameShellModule &) = delete;
        GameShellModule &operator=(const GameShellModule &) = delete;

    private:
        // what the player asked of the game, as requests App applies next frame
        void Apply(GameShellAction action);

        World &mWorld;
        GameShellSettings mSettings;
        GameShell mShell;
        UIDemoScreen mDemo;

        // last: unregisters before the screens above are destroyed
        Registration mScreen;
    };
}
