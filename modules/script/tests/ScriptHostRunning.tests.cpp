/**
 * @file ScriptHostRunning.tests.cpp
 * @author sumin.park
 * @brief Tests for ScriptHost's running entity, which a binding reads as "myself".
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <script/BindingApi.h>
#include <script/ScriptHost.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace
{
    constexpr std::string_view kOuter = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    record(who())
    call_inner()
    record(who())
end

return T
)LUA";

    constexpr std::string_view kInner = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    record(who())
end

return T
)LUA";
}

TEST_CASE("a binding sees the entity whose callback is running, also across a nested callback", "[script][host]")
{
    mir::World world;
    mir::CommandBuffer commands;
    mir::ScriptHost host;

    std::vector<mir::Entity> seen;
    const mir::Entity outer = world.CreateEntity();
    const mir::Entity inner = world.CreateEntity();
    int32_t innerInstance = -1;

    const mir::Registration bindings = mir::AddBindings(host,
        [&](sol::state &lua)
        {
            lua.set_function("who", [&lua]() { return mir::RunningScriptEntity(lua); });
            lua.set_function("record", [&seen](mir::Entity entity) { seen.push_back(entity); });
            lua.set_function("call_inner", [&]() { host.CallOnStart(world, commands, inner, innerInstance); });
        });

    REQUIRE(host.LoadScriptSource("outer", kOuter));
    REQUIRE(host.LoadScriptSource("inner", kInner));
    const int32_t outerInstance = host.CreateInstance("outer");
    innerInstance = host.CreateInstance("inner");

    host.CallOnStart(world, commands, outer, outerInstance);

    // before and after the nested callback it is the outer entity, inside it is the inner one
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == outer);
    CHECK(seen[1] == inner);
    CHECK(seen[2] == outer);
}
