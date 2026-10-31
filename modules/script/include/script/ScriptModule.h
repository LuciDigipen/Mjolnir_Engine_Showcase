/**
 * @file ScriptModule.h
 * @author sumin.park
 * @brief The Lua host, its systems and hot reload.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <script/ScriptHost.h>
#include <script/ScriptReloadWatcher.h>

#include <core/ecs/ModuleContext.h>
#include <core/Registration.h>

#include <string>

namespace mir
{
    class AssetResolver;
    class AssetsModule;
    class ScriptResolveSystem;
    class ScriptSystem;

    class ScriptModule
    {
    public:
        ScriptModule(ModuleContext context, AssetsModule &assets, const AssetResolver &resolver);

        ScriptModule(const ScriptModule &) = delete;
        ScriptModule &operator=(const ScriptModule &) = delete;

        ScriptHost &Host() { return mHost; }
        ScriptResolveSystem &Resolver() { return *mResolver; }
        ScriptSystem &Scripts() { return *mScripts; }

    private:
        ScriptHost mHost;
        ScriptReloadWatcher mReload{ mHost };

        // owned by the scheduler, which outlives this module
        ScriptResolveSystem *mResolver = nullptr;
        ScriptSystem *mScripts = nullptr;

        // last: they unregister before the host above is destroyed
        Registration mHooks;
        Registration mReloadPoll;
        Registration mPlayEnd;
        Registration mReloadCommit;
    };
}
