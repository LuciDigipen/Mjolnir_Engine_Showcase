/**
 * @file BindingApi.h
 * @author sumin.park
 * @brief How a bridge library adds its Lua bindings to the script host.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <script/BindingHelpers.h>

#include <core/Registration.h>
#include <core/ecs/Entity.h>

#include <sol/sol.hpp>

#include <cstdint>
#include <functional>
#include <vector>

// Only the script_bindings target sees this header, so sol2 stays out of every
// library but script and its bridges

namespace mir
{
    class ScriptHost;
    class World;

    using LuaBindings = std::function<void(sol::state &lua)>;

    // The entity whose callback is running on this Lua state, null between callbacks. A binding uses it for
    // what a script means by "myself".
    Entity RunningScriptEntity(sol::state_view lua);

    // Runs bindings on the host's Lua state now, and again on any state it builds
    // later. Unregisters when the token dies; what it already bound stays bound
    [[nodiscard]] Registration AddBindings(ScriptHost &host, LuaBindings bindings);

    // The host's Lua state, for a bridge that makes Lua values of its own to hand a callback
    sol::state_view LuaState(ScriptHost &host);

    // Calls name(self, world, entity, args...) on an instance, for a callback a bridge defines, such as OnUI, with
    // the running entity set as for OnUpdate. False when it failed, which is logged and counted as a script error,
    // true when it ran or the script has no such function
    bool CallScriptCallback(ScriptHost &host, World &world, Entity entity, int32_t instanceRef, const char *name,
        const std::vector<sol::object> &args = {});
}
