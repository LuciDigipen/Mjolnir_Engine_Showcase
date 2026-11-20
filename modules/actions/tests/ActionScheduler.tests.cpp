/**
 * @file ActionScheduler.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests entity and value actions, lanes, blocking, clocks and cancelling.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <actions/ActionScheduler.h>

#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <limits>

using namespace mir;

namespace
{
    struct Fixture
    {
        World world;
        ActionScheduler actions;

        Entity Spawn(const WorldVec3 &position = WorldVec3(0))
        {
            const Entity entity = world.CreateEntity();
            AddTransform(world, entity, Transform{ position });
            return entity;
        }

        const Transform &Of(Entity entity) { return *world.GetComponent<Transform>(entity); }

        void Tick(float dt, ActionClock clock = ActionClock::Game) { actions.Tick(world, clock, dt); }
    };
}

TEST_CASE("MoveTo eases an entity's position to its goal and then ends", "[actions]")
{
    Fixture f;
    const Entity box = f.Spawn();
    const ActionHandle move = f.actions.MoveTo(box, WorldVec3(10.0, 0.0, 0.0), { .mDuration = 1.0f });
    REQUIRE(move.IsValid());

    f.Tick(0.5f);
    CHECK(f.Of(box).Position().x == Catch::Approx(5.0));
    CHECK(f.actions.IsRunning(move));

    f.Tick(0.5f);
    CHECK(f.Of(box).Position().x == Catch::Approx(10.0));
    CHECK_FALSE(f.actions.IsRunning(move));
    CHECK(f.actions.Count() == 0);
}

TEST_CASE("The first tick moves the entity, it does not wait a frame", "[actions]")
{
    Fixture f;
    const Entity box = f.Spawn();
    f.actions.MoveTo(box, WorldVec3(4.0, 0.0, 0.0), { .mDuration = 1.0f });

    f.Tick(0.25f);
    CHECK(f.Of(box).Position().x == Catch::Approx(1.0));
}

TEST_CASE("RotateTo and ScaleTo reach their goals", "[actions]")
{
    Fixture f;
    const Entity box = f.Spawn();
    const glm::quat goal = glm::angleAxis(1.0f, glm::vec3(0.0f, 1.0f, 0.0f));
    f.actions.RotateTo(box, goal, { .mDuration = 0.5f });
    f.actions.ScaleTo(box, glm::vec3(2.0f, 3.0f, 4.0f), { .mDuration = 0.5f });

    f.Tick(0.5f);
    CHECK(std::abs(glm::dot(f.Of(box).Rotation(), goal)) == Catch::Approx(1.0f));
    CHECK(f.Of(box).Scale().y == Catch::Approx(3.0f));
}

TEST_CASE("The start value is read when the delay ends, not when the action is added", "[actions]")
{
    Fixture f;
    const Entity box = f.Spawn();
    f.actions.MoveTo(box, WorldVec3(10.0, 0.0, 0.0), { .mDuration = 1.0f, .mDelay = 1.0f });

    f.Tick(0.5f);
    CHECK(f.Of(box).Position().x == Catch::Approx(0.0));
    f.world.GetComponent<Transform>(box)->SetPosition(WorldVec3(6.0, 0.0, 0.0));

    // 0.5 finishes the delay, the other 0.5 is half the move from 6
    f.Tick(1.0f);
    CHECK(f.Of(box).Position().x == Catch::Approx(8.0));
}

TEST_CASE("A blocking action holds back later actions in its lane only", "[actions][lanes]")
{
    Fixture f;
    const Entity a = f.Spawn();
    const Entity b = f.Spawn();
    f.actions.MoveTo(a, WorldVec3(1.0, 0.0, 0.0), { .mDuration = 1.0f, .mBlocking = true });
    f.actions.ScaleTo(a, glm::vec3(2.0f), { .mDuration = 1.0f });
    f.actions.MoveTo(b, WorldVec3(1.0, 0.0, 0.0), { .mDuration = 1.0f });

    f.Tick(0.5f);
    CHECK(f.Of(a).Scale().x == Catch::Approx(1.0f));
    CHECK(f.Of(b).Position().x == Catch::Approx(0.5));

    // the move ends this tick, and the scale runs in the same tick
    f.Tick(0.5f);
    CHECK(f.Of(a).Position().x == Catch::Approx(1.0));
    CHECK(f.Of(a).Scale().x == Catch::Approx(1.5f));
}

TEST_CASE("A named lane sequences actions on different entities", "[actions][lanes]")
{
    Fixture f;
    const Entity a = f.Spawn();
    const Entity b = f.Spawn();
    const uint64_t cutscene = ActionLane("cutscene");
    f.actions.MoveTo(a, WorldVec3(1.0, 0.0, 0.0), { .mDuration = 1.0f, .mBlocking = true, .mLane = cutscene });
    f.actions.MoveTo(b, WorldVec3(1.0, 0.0, 0.0), { .mDuration = 1.0f, .mLane = cutscene });

    f.Tick(0.5f);
    CHECK(f.Of(b).Position().x == Catch::Approx(0.0));
    CHECK(f.actions.IsLaneBusy(cutscene));

    // a finishes and b runs in the same tick
    f.Tick(0.5f);
    CHECK(f.Of(a).Position().x == Catch::Approx(1.0));
    CHECK(f.Of(b).Position().x == Catch::Approx(0.5));
    f.Tick(0.5f);
    CHECK(f.Of(b).Position().x == Catch::Approx(1.0));
    CHECK_FALSE(f.actions.IsLaneBusy(cutscene));
}

TEST_CASE("A blocking action in its delay already blocks", "[actions][lanes]")
{
    Fixture f;
    const Entity box = f.Spawn();
    f.actions.MoveTo(box, WorldVec3(1.0, 0.0, 0.0), { .mDuration = 1.0f, .mDelay = 1.0f, .mBlocking = true });
    f.actions.ScaleTo(box, glm::vec3(3.0f), { .mDuration = 0.0f });

    f.Tick(0.5f);
    CHECK(f.Of(box).Scale().x == Catch::Approx(1.0f));
}

TEST_CASE("A zero duration lands on its goal on the first tick", "[actions]")
{
    Fixture f;
    const Entity box = f.Spawn();
    const ActionHandle snap = f.actions.MoveTo(box, WorldVec3(3.0, 2.0, 1.0), { .mDuration = 0.0f });

    f.Tick(0.0f);
    CHECK(f.Of(box).Position().y == Catch::Approx(2.0));
    CHECK_FALSE(f.actions.IsRunning(snap));
}

TEST_CASE("An action whose entity is destroyed ends quietly", "[actions]")
{
    Fixture f;
    const Entity box = f.Spawn();
    const ActionHandle move = f.actions.MoveTo(box, WorldVec3(1.0, 0.0, 0.0), { .mDuration = 1.0f });
    f.Tick(0.25f);
    f.world.DestroyEntity(box);

    f.Tick(0.25f);
    CHECK_FALSE(f.actions.IsRunning(move));
    CHECK(f.actions.Count() == 0);
}

TEST_CASE("Each clock advances only its own actions", "[actions][clock]")
{
    Fixture f;
    const Entity box = f.Spawn();
    f.actions.MoveTo(box, WorldVec3(1.0, 0.0, 0.0), { .mDuration = 1.0f });
    f.actions.TweenValue("Menu.alpha", 1.0f, { .mDuration = 1.0f, .mClock = ActionClock::UI });

    f.Tick(0.5f, ActionClock::UI);
    CHECK(f.Of(box).Position().x == Catch::Approx(0.0));
    CHECK(f.actions.Value("Menu.alpha") == Catch::Approx(0.5f));

    f.Tick(0.5f, ActionClock::Game);
    CHECK(f.Of(box).Position().x == Catch::Approx(0.5));
    CHECK(f.actions.Value("Menu.alpha") == Catch::Approx(0.5f));
}

TEST_CASE("A value tween starts from what the key holds, and an unset key holds 0", "[actions][values]")
{
    Fixture f;
    CHECK_FALSE(f.actions.HasValue("Title.slide"));
    CHECK(f.actions.Value("Title.slide", 7.0f) == 7.0f);

    f.actions.TweenValue("Title.slide", 10.0f, { .mDuration = 1.0f });
    f.Tick(0.5f);
    CHECK(f.actions.Value("Title.slide") == Catch::Approx(5.0f));

    f.actions.SetValue("Title.alpha", 1.0f);
    f.actions.TweenValue("Title.alpha", 0.0f, { .mDuration = 1.0f });
    f.Tick(0.25f);
    CHECK(f.actions.Value("Title.alpha") == Catch::Approx(0.75f));
}

TEST_CASE("TweenVec4 eases all four components", "[actions][values]")
{
    Fixture f;
    f.actions.SetVec4("Panel.rect", glm::vec4(0.0f, 0.0f, 100.0f, 40.0f));
    f.actions.TweenVec4("Panel.rect", glm::vec4(20.0f, 10.0f, 200.0f, 80.0f), { .mDuration = 1.0f });

    f.Tick(0.5f);
    const glm::vec4 rect = f.actions.Vec4("Panel.rect");
    CHECK(rect.x == Catch::Approx(10.0f));
    CHECK(rect.y == Catch::Approx(5.0f));
    CHECK(rect.z == Catch::Approx(150.0f));
    CHECK(rect.w == Catch::Approx(60.0f));
}

TEST_CASE("Tweens of one value run in that value's lane", "[actions][values][lanes]")
{
    Fixture f;
    f.actions.TweenValue("Toast.alpha", 1.0f, { .mDuration = 1.0f, .mBlocking = true });
    f.actions.TweenValue("Toast.alpha", 0.0f, { .mDuration = 1.0f });
    CHECK(f.actions.IsLaneBusy(ActionLane("Toast.alpha")));

    f.Tick(0.5f);
    CHECK(f.actions.Value("Toast.alpha") == Catch::Approx(0.5f));

    // the fade in ends at 1, and the fade out starts from there with the whole tick
    f.Tick(0.75f);
    CHECK(f.actions.Value("Toast.alpha") == Catch::Approx(0.25f));
    f.Tick(0.75f);
    CHECK(f.actions.Value("Toast.alpha") == Catch::Approx(0.0f));
}

TEST_CASE("Time scales multiply the clock and the lane", "[actions][clock]")
{
    Fixture f;
    f.actions.SetTimeScale(ActionClock::Game, 2.0f);
    f.actions.SetLaneTimeScale(ActionLane("Bar"), 0.5f);
    f.actions.TweenValue("Bar", 1.0f, { .mDuration = 1.0f });
    f.actions.TweenValue("Other", 1.0f, { .mDuration = 1.0f });

    f.Tick(0.25f);
    CHECK(f.actions.Value("Bar") == Catch::Approx(0.25f));
    CHECK(f.actions.Value("Other") == Catch::Approx(0.5f));
}

TEST_CASE("A paused lane keeps its actions where they are", "[actions][lanes]")
{
    Fixture f;
    const uint64_t lane = ActionLane("Spinner");
    f.actions.TweenValue("Spinner", 1.0f, { .mDuration = 1.0f });
    f.actions.SetLanePaused(lane, true);
    CHECK(f.actions.IsLanePaused(lane));

    f.Tick(0.5f);
    CHECK(f.actions.Value("Spinner", -1.0f) == -1.0f);

    f.actions.SetLanePaused(lane, false);
    f.Tick(0.5f);
    CHECK(f.actions.Value("Spinner") == Catch::Approx(0.5f));
}

TEST_CASE("Cancelling leaves the target where it got to", "[actions]")
{
    Fixture f;
    const Entity box = f.Spawn();
    const ActionHandle move = f.actions.MoveTo(box, WorldVec3(2.0, 0.0, 0.0), { .mDuration = 1.0f });
    f.actions.ScaleTo(box, glm::vec3(2.0f), { .mDuration = 1.0f });
    f.actions.TweenValue("Fade", 1.0f, { .mDuration = 1.0f });
    f.Tick(0.5f);

    f.actions.Cancel(move);
    CHECK_FALSE(f.actions.IsRunning(move));
    CHECK(f.actions.Count() == 2);

    f.actions.CancelAll(box);
    CHECK(f.actions.Count() == 1);
    f.actions.CancelValue("Fade");
    CHECK(f.actions.Count() == 0);

    f.Tick(0.5f);
    CHECK(f.Of(box).Position().x == Catch::Approx(1.0));
    CHECK(f.actions.Value("Fade") == Catch::Approx(0.5f));
}

TEST_CASE("Bad input is refused with an invalid handle", "[actions]")
{
    Fixture f;
    const Entity box = f.Spawn();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    CHECK_FALSE(f.actions.MoveTo(kNullEntity, WorldVec3(1.0)).IsValid());
    CHECK_FALSE(f.actions.MoveTo(box, WorldVec3(1.0), { .mDuration = -1.0f }).IsValid());
    CHECK_FALSE(f.actions.MoveTo(box, WorldVec3(1.0), { .mDelay = nan }).IsValid());
    CHECK_FALSE(f.actions.RotateTo(box, glm::quat(0.0f, 0.0f, 0.0f, 0.0f)).IsValid());
    CHECK_FALSE(f.actions.ScaleTo(box, glm::vec3(nan)).IsValid());
    CHECK_FALSE(f.actions.TweenValue("", 1.0f).IsValid());
    CHECK(f.actions.Count() == 0);

    f.actions.SetTimeScale(ActionClock::UI, -1.0f);
    CHECK(f.actions.TimeScale(ActionClock::UI) == 1.0f);
}

TEST_CASE("Clear drops every action, value and setting", "[actions]")
{
    Fixture f;
    f.actions.TweenValue("A", 1.0f);
    f.actions.SetValue("B", 2.0f);
    f.actions.SetLanePaused(ActionLane("A"), true);
    f.actions.SetTimeScale(ActionClock::Game, 3.0f);

    f.actions.Clear();
    CHECK(f.actions.Count() == 0);
    CHECK_FALSE(f.actions.HasValue("B"));
    CHECK_FALSE(f.actions.IsLanePaused(ActionLane("A")));
    CHECK(f.actions.TimeScale(ActionClock::Game) == 1.0f);
}

TEST_CASE("A dead target's blocking action frees its lane during its delay", "[actions][lanes]")
{
    Fixture f;
    const Entity box = f.Spawn();
    f.actions.MoveTo(box, WorldVec3(1.0, 0.0, 0.0), { .mDuration = 1.0f, .mDelay = 5.0f, .mBlocking = true });
    f.actions.TweenValue("After", 1.0f, { .mDuration = 1.0f, .mLane = ActionLane(box) });
    f.world.DestroyEntity(box);

    f.Tick(0.5f);
    CHECK(f.actions.Value("After") == Catch::Approx(0.5f));
    CHECK(f.actions.Count() == 1);
}

TEST_CASE("A clock or lane scale of 0 freezes actions, and a zero dt at a live scale still runs instant ones", "[actions][clock]")
{
    Fixture f;

    SECTION("Clock scale 0")
    {
        f.actions.SetTimeScale(ActionClock::Game, 0.0f);
        f.actions.TweenValue("Snap", 1.0f, { .mDuration = 0.0f });
        f.Tick(0.5f);
        CHECK_FALSE(f.actions.HasValue("Snap"));
    }

    SECTION("Lane scale 0 reads no start value until it thaws")
    {
        const uint64_t lane = ActionLane("Frozen");
        f.actions.SetLaneTimeScale(lane, 0.0f);
        f.actions.TweenValue("Frozen", 10.0f, { .mDuration = 1.0f });
        f.Tick(0.5f);
        f.actions.SetValue("Frozen", 4.0f);

        f.actions.SetLaneTimeScale(lane, 1.0f);
        f.Tick(0.5f);
        CHECK(f.actions.Value("Frozen") == Catch::Approx(7.0f));
    }

    SECTION("Zero dt at scale 1")
    {
        f.actions.TweenValue("Snap", 1.0f, { .mDuration = 0.0f });
        f.Tick(0.0f);
        CHECK(f.actions.Value("Snap") == 1.0f);
    }
}

TEST_CASE("A tween ends exactly on its goal and never overflows between far-apart values", "[actions][values]")
{
    Fixture f;
    const float max = std::numeric_limits<float>::max();

    f.actions.SetValue("Far", 100000000.0f);
    f.actions.TweenValue("Far", 1.0f, { .mDuration = 1.0f });
    f.actions.SetValue("Wide", -max);
    f.actions.TweenValue("Wide", max, { .mDuration = 1.0f });

    f.Tick(0.5f);
    CHECK(std::isfinite(f.actions.Value("Wide")));
    CHECK(f.actions.Value("Wide") == Catch::Approx(0.0f).margin(1.0f));

    f.Tick(0.5f);
    CHECK(f.actions.Value("Far") == 1.0f);
    CHECK(f.actions.Value("Wide") == max);
}

TEST_CASE("Setters say whether they took the value", "[actions]")
{
    Fixture f;
    const float inf = std::numeric_limits<float>::infinity();

    CHECK(f.actions.SetValue("A", 1.0f));
    CHECK_FALSE(f.actions.SetValue("A", inf));
    CHECK_FALSE(f.actions.SetValue("", 1.0f));
    CHECK_FALSE(f.actions.SetVec4("B", glm::vec4(inf)));
    CHECK(f.actions.SetTimeScale(ActionClock::Game, 0.0f));
    CHECK_FALSE(f.actions.SetTimeScale(ActionClock::Game, inf));
    CHECK_FALSE(f.actions.SetLaneTimeScale(ActionLane("C"), -1.0f));
    CHECK(f.actions.Value("A") == 1.0f);
}
