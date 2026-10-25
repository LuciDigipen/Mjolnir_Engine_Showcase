/**
 * @file ScriptModule.cpp
 * @author sumin.park
 * @brief The Lua host, its systems and hot reload.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <script/ScriptModule.h>

#include <script/ComponentRegistration.h>
#include <script/ScriptResolveSystem.h>
#include <script/ScriptSystem.h>
#include <script/components/ScriptRef.h>

#include <assets/AssetsModule.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>

#include <utility>

namespace mir
{
    ScriptModule::ScriptModule(ModuleContext context, AssetsModule &assets, std::string assetIdRoot)
    {
        RegisterScriptComponents();

        // the resolver first, so a ScriptRef made last frame runs this frame
        mResolver = &context.scheduler.AddSystem<ScriptResolveSystem>(SystemPhase::PreUpdate,
            SystemOrder{SystemOrder::kEarly}, mHost,
            MakeScriptAssetLoader(mHost, mReload, assets.Cache(), std::move(assetIdRoot)));
        mScripts = &context.scheduler.AddSystem<ScriptSystem>(SystemPhase::PreUpdate,
            SystemOrder{SystemOrder::kNormal}, mHost);

        // a copy arrives with the original's bytes, so it starts without an instance and not yet started
        mHooks = context.world.SetComponentHooks<ScriptRef>({
            .onAdd = [](World &world, Entity entity)
            {
                ScriptRef &ref = *world.GetComponent<ScriptRef>(entity);
                ref.instanceRef = -1;
                ref.started = false;
            },
            .onRemove = [this](World &world, Entity entity)
            {
                ReleaseScriptInstance(mHost, *world.GetComponent<ScriptRef>(entity));
            } });

        mReloadPoll = context.scheduler.AddFrameSlot(FrameSlot::FrameBegin, [this, &assets](SystemContext &frame)
            {
                mReload.Poll(&assets.Cache(), frame.dt);
            });

        // before Stop's restore destroys the entities the scripts run on
        mPlayEnd = context.scheduler.AddStage(Stage::PlayEnd, [this](SystemContext &frame) { mScripts->StopAllScripts(frame); });

        // a script whose asset arrived with this cook resolves now
        mReloadCommit = context.scheduler.AddStage(Stage::ReloadCommit, [this](SystemContext &) { mResolver->ForgetFailures(); });
    }
}
