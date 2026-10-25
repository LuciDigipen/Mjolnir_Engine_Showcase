/**
 * @file ScriptFixedSystem.h
 * @author sumin.park
 * @brief Dispatches OnFixedUpdate to every started script, once per fixed step.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <script/components/ScriptRef.h>

#include <core/ecs/Query.h>
#include <core/ecs/System.h>

namespace mir
{
    class IScriptHost;

    // FixedUpdate, before physics, so forces and velocities a script sets here act on this step.
    // A script whose OnStart has not run yet is skipped, so OnStart always comes first
    class ScriptFixedSystem final : public ISystem
    {
    public:
        explicit ScriptFixedSystem(IScriptHost &host) : mHost(host)
        {}

        void OnStart(SystemContext &context) override;
        void OnUpdate(SystemContext &context) override;

    private:
        IScriptHost &mHost;
        Query<ScriptRef> *mQuery = nullptr;
    };
}
