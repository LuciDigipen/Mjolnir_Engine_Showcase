/**
 * @file GameShellModule.cpp
 * @author sumin.park
 * @brief Registers the HUD and pause menu, or the widget demo while GameUIDemo asks for it,
 *        as the game UI's screen.
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
#include <gameui/GameUIDemo.h>
#include <gameui/GameUIModule.h>

namespace mir
{
    GameShellModule::GameShellModule(ModuleContext context, GameUIModule &gameUI, AssetsModule &assets,
        const AssetResolver &resolver, const GameShellSettings &settings)
        : mWorld(context.world), mAssets(assets), mResolver(resolver), mSettings(settings)
    {
        if (GameUIDemo *demo = mWorld.TryResource<GameUIDemo>())
            demo->mShow = mSettings.mUIDemo;

        mScreen = gameUI.AddScreen([this](UIContext &ui, float dt) { Draw(ui, dt); });
    }

    void GameShellModule::Draw(UIContext &ui, float dt)
    {
        const GameUIDemo *demo = mWorld.TryResource<GameUIDemo>();
        const bool showDemo = demo != nullptr && demo->mShow;

        if (showDemo != mDemoShown)
        {
            if (showDemo)
                mShellStyle = ui.Style();
            else
                ui.Style() = mShellStyle;
            mDemoShown = showDemo;
        }

        if (!showDemo)
        {
            Apply(mShell.Draw(ui, IsPlaying(mWorld), dt));
            return;
        }

        // loaded on first show, so a run that never asks for it never reads its art
        if (!mDemoLoaded)
        {
            mDemo.Load(ui, &mAssets.Cache(), mResolver);
            mDemoLoaded = true;
        }
        mDemo.Draw(ui);
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
