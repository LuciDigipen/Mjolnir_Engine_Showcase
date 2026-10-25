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

#include <functional>

// Only the script_bindings target sees this header, so sol2 stays out of every
// library but script and its bridges

namespace mir
{
    class ScriptHost;

    using LuaBindings = std::function<void(sol::state &lua)>;

    // The entity whose callback is running on this Lua state, null between callbacks. A binding uses it for
    // what a script means by "myself".
    Entity RunningScriptEntity(sol::state_view lua);

    // Runs bindings on the host's Lua state now, and again on any state it builds
    // later. Unregisters when the token dies; what it already bound stays bound
    [[nodiscard]] Registration AddBindings(ScriptHost &host, LuaBindings bindings);
}
