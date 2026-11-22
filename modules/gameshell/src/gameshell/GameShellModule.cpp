/**
 * @file GameShellModule.cpp
 * @author sumin.park
 * @brief Registers the HUD and pause menu, or the widget demo while GameUIDemo asks for it,
 *        as the game UI's screen, and loads the game's UI style file as the game UI's base style.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameshell/GameShellModule.h>

#include <gameshell/UIStyleFile.h>

#include <assets/AssetBlob.h>
#include <assets/AssetCache.h>
#include <assets/AssetResolver.h>
#include <assets/AssetsModule.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/Requests.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>
#include <gameui/GameUIDemo.h>
#include <gameui/GameUIModule.h>

#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    namespace
    {
        // ui's base style with the file's fields over it, a file that does not load is the base style unchanged
        UIStyle LoadStyle(UIContext &ui, AssetCache &cache, const AssetResolver &resolver, std::string_view path)
        {
            std::string error;
            const AssetId id = resolver.Resolve(path, &error);
            const AssetBlobView *blob = id.IsNull() ? nullptr : cache.Load(id);
            if (blob == nullptr)
            {
                MIR_LOG_ERROR("ui style '{}': {}", path, id.IsNull() ? error : "did not load");
                return ui.BaseStyle();
            }

            const auto find = [&](std::string_view file)
            {
                std::string unused;
                return resolver.Resolve(file, &unused);
            };
            const UIStyleAssets assets{
                .mSprite = [&](std::string_view file)
                {
                    const AssetId sprite = find(file);
                    return sprite.IsNull() ? UISprite{} : ui.Textures().Load(cache, sprite);
                },
                .mFont = [&](std::string_view file)
                {
                    const AssetId font = find(file);
                    return font.IsNull() ? UIFont{} : ui.Fonts().Load(cache, font);
                },
            };
            std::vector<std::string> errors;
            const std::string_view text(reinterpret_cast<const char *>(blob->content.data()), blob->content.size());
            const UIStyle style = ParseUIStyle(text, ui.BaseStyle(), assets, errors);
            for (const std::string &problem : errors)
                MIR_LOG_ERROR("ui style '{}': {}", path, problem);
            return style;
        }
    } // namespace

    GameShellModule::GameShellModule(ModuleContext context, GameUIModule &gameUI, AssetsModule &assets,
        const AssetResolver &resolver, const GameShellSettings &settings)
        : mWorld(context.world), mAssets(assets), mResolver(resolver), mSettings(settings)
    {
        if (GameUIDemo *demo = mWorld.TryResource<GameUIDemo>())
            demo->mShow = mSettings.mUIDemo;
        mShell.SetParts(mSettings.mParts);
        if (!mSettings.mStyle.empty())
            gameUI.Context().SetBaseStyle(LoadStyle(gameUI.Context(), mAssets.Cache(), mResolver, mSettings.mStyle));

        // here rather than in Draw, which does not run while the demo shows
        mPlayBegin = context.scheduler.AddStage(Stage::PlayBegin, [this](SystemContext &) { mShell.Reset(); });
        mPlayEnd = context.scheduler.AddStage(Stage::PlayEnd, [this](SystemContext &) { mShell.Reset(); });

        mScreen = gameUI.AddScreen(UIScreenLayer::Shell, [this](UIContext &ui, float dt) { Draw(ui, dt); });
    }

    void GameShellModule::Draw(UIContext &ui, float dt)
    {
        const GameUIDemo *demo = mWorld.TryResource<GameUIDemo>();
        const bool showDemo = demo != nullptr && demo->mShow;

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
            // App turns it into Stop in the editor
            requests.Push(Request::Quit);
            break;
        case GameShellAction::None:
            break;
        }
    }
}
