/**
 * @file GameShellModule.cpp
 * @author sumin.park
 * @brief Registers the HUD and pause menu, or the widget demo, as the game UI's screen.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameshell/GameShellModule.h>

#include <assets/AssetResolver.h>
#include <assets/AssetsModule.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/Requests.h>
#include <core/ecs/World.h>
#include <gameui/GameUIModule.h>

namespace mir
{
    GameShellModule::GameShellModule(ModuleContext context, GameUIModule &gameUI, AssetsModule &assets,
        const AssetResolver &resolver, const GameShellSettings &settings)
        : mWorld(context.world), mSettings(settings)
    {
        if (mSettings.mUIDemo)
        {
            mDemo.Load(gameUI.Context(), &assets.Cache(), resolver);
            mScreen = gameUI.AddScreen([this](UIContext &ui, float) { mDemo.Draw(ui); });
            return;
        }

        mScreen = gameUI.AddScreen([this](UIContext &ui, float dt)
            {
                Apply(mShell.Draw(ui, IsPlaying(mWorld), dt));
            });
    }

    void GameShellModule::Apply(GameShellAction action)
    {
        RequestQueue &requests = mWorld.Resource<RequestQueue>();
        switch (action)
        {
        case GameShellAction::Pause:
            requests.Push(Request::Pause);
            break;
        case GameShellAction::Resume:
            requests.Push(Request::Resume);
            break;
        case GameShellAction::Restart:
            requests.Push(Request::Stop);
            requests.Push(Request::Play);
            break;
        case GameShellAction::Quit:
            requests.Push(mSettings.mQuitStopsPlay ? Request::Stop : Request::Quit);
            break;
        case GameShellAction::None:
            break;
        }
    }
}
