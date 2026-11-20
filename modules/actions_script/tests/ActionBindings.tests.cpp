/**
 * @file ActionBindings.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests the Lua action calls against a real ActionScheduler.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <actions_script/ActionBindings.h>
#include <script/BindingApi.h>
#include <script/ScriptHost.h>

#include <actions/ActionScheduler.h>
#include <core/ecs/CommandBuffer.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>

using namespace mir;

namespace
{
    const ComponentOps &ProbeOps()
    {
        static const RuntimeFieldDecl fields[] = {
            {"handleValid", FieldKind::Bool},
            {"running", FieldKind::Bool},
            {"laneBusy", FieldKind::Bool},
            {"badOption", FieldKind::Bool},
            {"badEase", FieldKind::Bool},
            {"badGoal", FieldKind::Bool},
            {"score", FieldKind::Float},
            {"missing", FieldKind::Float},
        };
        return ComponentRegistry::Instance().RegisterRuntime("ActionBindingsProbe", fields);
    }

    template <typename T>
    T ReadProbeField(const ComponentOps &probe, World &world, Entity entity, std::string_view field)
    {
        T value{};
        probe.FindField(field)->Read(probe.GetComponent(world, entity), &value);
        return value;
    }

    constexpr std::string_view kQueueSource = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local move = world:move_to(entity, { x = 4, y = 0, z = 0 }, { duration = 1, ease = "Linear", blocking = true })
    world:scale_to(entity, { x = 2, y = 2, z = 2 }, { duration = 1 })
    world:set(entity, "ActionBindingsProbe", "handleValid", move ~= nil)
    world:set(entity, "ActionBindingsProbe", "running", world:action_running(move))
    world:set(entity, "ActionBindingsProbe", "laneBusy", world:lane_busy(entity))

    world:set(entity, "ActionBindingsProbe", "badOption", world:move_to(entity, { x = 1, y = 0, z = 0 }, { durration = 1 }) == nil)
    world:set(entity, "ActionBindingsProbe", "badEase", world:move_to(entity, { x = 1, y = 0, z = 0 }, { ease = "Sideways" }) == nil)
    world:set(entity, "ActionBindingsProbe", "badGoal", world:move_to(entity, { 1, 2, 3 }) == nil)

    world:set_value("Menu.alpha", 0)
    world:tween_value("Menu.alpha", 1, { duration = 1, clock = "ui" })
    world:set_value("Score", 3)
    world:set(entity, "ActionBindingsProbe", "score", world:value("Score"))
    world:set(entity, "ActionBindingsProbe", "missing", world:value("Nope", 9))
end

return T
)LUA";

    constexpr std::string_view kControlSource = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local move = world:move_to(entity, { x = 4, y = 0, z = 0 }, { duration = 1 })
    world:cancel_action(move)
    world:rotate_to(entity, { x = 0, y = 0.7071068, z = 0, w = 0.7071068 }, { duration = 1, lane = "spin" })
    world:set_lane_paused("spin", true)
    world:tween_value("Fast", 1, { duration = 1 })
    world:set_action_time_scale(2)
end

return T
)LUA";

    struct Fixture
    {
        const ComponentOps &probe = ProbeOps();
        World world;
        CommandBuffer commands;
        ActionScheduler &actions = world.EmplaceResource<ActionScheduler>();
        ScriptHost host;
        Registration bindings = AddBindings(host, &RegisterActionBindings);
        Entity entity;

        Fixture()
        {
            entity = world.CreateEntity();
            AddTransform(world, entity);
            probe.AddDefault(world, entity);
        }

        void Run(std::string_view name, std::string_view source)
        {
            REQUIRE(host.LoadScriptSource(name, source));
            const int32_t instance = host.CreateInstance(name);
            REQUIRE(instance != -1);
            host.CallOnStart(world, commands, entity, instance);
            commands.Flush(world);
            host.DestroyInstance(instance);
        }

        const Transform &Pose() { return *world.GetComponent<Transform>(entity); }
    };
}

TEST_CASE("Lua queues entity and value actions and reads values back", "[script][actions]")
{
    Fixture f;
    f.Run("action_queue", kQueueSource);

    CHECK(ReadProbeField<bool>(f.probe, f.world, f.entity, "handleValid"));
    CHECK(ReadProbeField<bool>(f.probe, f.world, f.entity, "running"));
    CHECK(ReadProbeField<bool>(f.probe, f.world, f.entity, "laneBusy"));
    CHECK(ReadProbeField<bool>(f.probe, f.world, f.entity, "badOption"));
    CHECK(ReadProbeField<bool>(f.probe, f.world, f.entity, "badEase"));
    CHECK(ReadProbeField<bool>(f.probe, f.world, f.entity, "badGoal"));
    CHECK(ReadProbeField<float>(f.probe, f.world, f.entity, "score") == 3.0f);
    CHECK(ReadProbeField<float>(f.probe, f.world, f.entity, "missing") == 9.0f);
    CHECK(f.actions.Count() == 3);

    // the move blocks the scale, which shares the entity's lane
    f.actions.Tick(f.world, ActionClock::Game, 0.5f);
    CHECK(f.Pose().Position().x == Catch::Approx(2.0));
    CHECK(f.Pose().Scale().x == Catch::Approx(1.0f));
    CHECK(f.actions.Value("Menu.alpha") == Catch::Approx(0.0f));

    f.actions.Tick(f.world, ActionClock::UI, 0.25f);
    CHECK(f.actions.Value("Menu.alpha") == Catch::Approx(0.25f));
}

TEST_CASE("Lua cancels actions, pauses lanes and scales a clock", "[script][actions]")
{
    Fixture f;
    f.Run("action_control", kControlSource);

    CHECK(f.actions.Count() == 2);
    CHECK(f.actions.IsLanePaused(ActionLane("spin")));
    CHECK(f.actions.TimeScale(ActionClock::Game) == 2.0f);

    f.actions.Tick(f.world, ActionClock::Game, 0.25f);
    CHECK(f.Pose().Position().x == Catch::Approx(0.0));
    CHECK(f.Pose().Rotation().w == Catch::Approx(1.0f));
    CHECK(f.actions.Value("Fast") == Catch::Approx(0.5f));
}
