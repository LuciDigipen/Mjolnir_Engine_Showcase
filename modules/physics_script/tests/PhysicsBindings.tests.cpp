/**
 * @file PhysicsBindings.tests.cpp
 * @author Jackey Cen
 * @brief Tests for the Lua world:add_force/add_torque/velocity bindings.
 *
 * No window and no renderer: a bare World plus the slice of App::Initialize a
 * PhysicsSystem needs, with ScriptHost driven by hand where App would have run
 * a ScriptSystem.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics_script/PhysicsBindings.h>
#include <script/BindingApi.h>
#include <script/ComponentRegistration.h>
#include <script/ScriptFixedSystem.h>
#include <script/ScriptHost.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <physics/ComponentRegistration.h>
#include <physics/PhysicsSystem.h>
#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr float kStep = 1.0f / 60.0f;

    // Anything below this counts as "did not move". Loose enough to absorb how
    // Jolt spreads a push across a substep, tight enough that the axis a test
    // does expect to move is never mistaken for noise.
    constexpr float kStill = 1.0e-3f;

    // The mass of a unit cube at Jolt's default density, which is what the forces below were chosen for.
    constexpr mir::RigidBody kDynamicBox{
        .motion = mir::PhysicsMotion::Dynamic,
        .shape = mir::PhysicsShape::Box,
        .halfExtent = glm::vec3(0.5f),
        .mass = 1000.0f,
    };

    // Where a script writes what it read, so the test can check it. Same trick
    // as InputBindings.tests.cpp.
    const mir::ComponentOps &ProbeOps()
    {
        static const mir::RuntimeFieldDecl fields[] = {
            { "linear", mir::FieldKind::Vec3 },
            { "angular", mir::FieldKind::Vec3 },
            { "ok", mir::FieldKind::Bool },
        };
        return mir::ComponentRegistry::Instance().RegisterRuntime("PhysicsBindingsProbe", fields);
    }

    template <typename T>
    T ReadProbeField(mir::World &world, mir::Entity entity, std::string_view field)
    {
        const mir::ComponentOps &probe = ProbeOps();
        T value{};
        probe.FindField(field)->Read(probe.GetComponent(world, entity), &value);
        return value;
    }

    struct Fixture
    {
        mir::World world;
        mir::CommandBuffer commands;
        mir::SystemScheduler scheduler;
        mir::SystemContext context{ world, commands, kStep, 0.0, 0 };
        mir::ScriptHost host;
        const mir::Registration bindings = mir::AddBindings(host, &mir::RegisterPhysicsBindings);
        // what PhysicsModule sets, so the body exists as soon as the RigidBody does
        mir::Registration hooks;
        mir::Entity entity{};
        int32_t instance = -1;

        Fixture(std::string_view source, bool withRigidBody = true)
        {
            // No gravity: an applied push is then the only thing moving the
            // body, so every assertion below can be about that push alone.
            world.EmplaceResource<mir::PhysicsWorld>(mir::PhysicsWorldDesc{ .mGravity = glm::vec3(0.0f) });
            Physics().SetSimulationLive(true);
            hooks = world.SetComponentHooks<mir::RigidBody>(
                { .onAdd = &mir::PhysicsSystem::OnRigidBodyAdded, .onRemove = &mir::PhysicsSystem::OnRigidBodyRemoved });
            world.EmplaceResource<mir::EngineModeState>();
            mir::SetEngineMode(world, mir::EngineMode::Play);
            mir::SetPlayState(world, mir::PlayState::Running);
            mir::RegisterScriptComponents();
            mir::RegisterPhysicsComponents();

            // the same order ScriptModule and PhysicsModule give them: scripts first
            scheduler.AddSystem<mir::ScriptFixedSystem>(mir::SystemPhase::FixedUpdate,
                mir::SystemOrder{ mir::SystemOrder::kEarly }, host);
            scheduler.AddSystem<mir::PhysicsSystem>(mir::SystemPhase::FixedUpdate, mir::SystemOrder{mir::SystemOrder::kNormal});
            scheduler.Start(context);

            entity = world.CreateEntity();
            mir::AddTransform(world, entity, mir::Transform{});
            if (withRigidBody)
                world.AddComponent<mir::RigidBody>(entity, kDynamicBox);
            ProbeOps().AddDefault(world, entity);

            const std::string name = "physics_probe";
            REQUIRE(host.LoadScriptSource(name, source));
            instance = host.CreateInstance(name);
            REQUIRE(instance != -1);

            // PhysicsSystem builds the body on its first simulating substep, so
            // nothing can push at it before this.
            Step();
        }

        ~Fixture()
        {
            host.DestroyInstance(instance);
            if (scheduler.Started())
                scheduler.Stop(context);
        }

        mir::PhysicsWorld &Physics() { return world.Resource<mir::PhysicsWorld>(); }

        mir::BodyHandle Body() { return world.GetComponent<mir::RigidBody>(entity)->runtime.body; }

        glm::vec3 Linear() { return Physics().BodyLinearVelocity(Body()); }
        glm::vec3 Angular() { return Physics().BodyAngularVelocity(Body()); }

        void Start()
        {
            host.CallOnStart(world, commands, entity, instance);
            commands.Flush(world);
        }

        // The script half of a frame on its own, for reading the state the last
        // step left behind without adding another one.
        void Script()
        {
            host.CallOnUpdate(world, commands, entity, instance, kStep);
            commands.Flush(world);
        }

        void Step() { scheduler.Update(context, 1, kStep); }

        // One frame the way App::Run drives it: the script's PreUpdate first,
        // then one fixed substep of PhysicsSystem.
        void Frames(int count)
        {
            for (int i = 0; i < count; ++i)
            {
                Script();
                Step();
            }
        }
    };

    constexpr std::string_view kPushForward = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnUpdate(self, world, entity, dt)
    world:add_force(entity, { x = 0.0, y = 0.0, z = 5000.0 })
end

return T
)LUA";

    constexpr std::string_view kPushForwardAtOffset = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnUpdate(self, world, entity, dt)
    world:add_force(entity, { x = 0.0, y = 0.0, z = 1000.0 }, { x = 1.0, y = 0.0, z = 0.0 })
end

return T
)LUA";

    constexpr std::string_view kTwist = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnUpdate(self, world, entity, dt)
    world:add_torque(entity, { x = 0.0, y = 500.0, z = 0.0 })
end

return T
)LUA";

    // Pushes and records in the same call, so the recorded value is whatever the
    // bindings saw at that moment.
    constexpr std::string_view kPushAndRecord = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnUpdate(self, world, entity, dt)
    world:set(entity, "PhysicsBindingsProbe", "linear", world:linear_velocity(entity))
    world:set(entity, "PhysicsBindingsProbe", "angular", world:angular_velocity(entity))
    world:add_force(entity, { x = 0.0, y = 0.0, z = 5000.0 }, { x = 1.0, y = 0.0, z = 0.0 })
end

return T
)LUA";

    constexpr std::string_view kWriteVelocity = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    world:set_linear_velocity(entity, { x = 1.0, y = 2.0, z = 3.0 })
    world:add_linear_velocity(entity, { x = 1.0, y = 0.0, z = 0.0 })
    world:set_angular_velocity(entity, { x = 0.0, y = 4.0, z = 0.0 })
    world:add_angular_velocity(entity, { x = 0.0, y = 1.0, z = 0.0 })
end

return T
)LUA";

    constexpr std::string_view kImpulses = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    world:add_impulse(entity, { x = 0.0, y = 0.0, z = 1000.0 })
    world:add_angular_impulse(entity, { x = 0.0, y = 100.0, z = 0.0 })
end

return T
)LUA";

    constexpr std::string_view kOffsetImpulse = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    world:add_impulse(entity, { x = 0.0, y = 0.0, z = 1000.0 }, { x = 1.0, y = 0.0, z = 0.0 })
end

return T
)LUA";

    // `or` short-circuits, so anySucceeded stays false only while every writer
    // refuses. ok is the negation so that a script that died on the way here
    // leaves it at its default false and fails the test rather than passing it.
    constexpr std::string_view kProbeEverything = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local v = { x = 1.0, y = 2.0, z = 3.0 }

    local anySucceeded = world:add_force(entity, v)
        or world:add_force(entity, v, v)
        or world:add_torque(entity, v)
        or world:add_impulse(entity, v)
        or world:add_impulse(entity, v, v)
        or world:add_angular_impulse(entity, v)
        or world:set_linear_velocity(entity, v)
        or world:set_angular_velocity(entity, v)
        or world:add_linear_velocity(entity, v)
        or world:add_angular_velocity(entity, v)

    world:set(entity, "PhysicsBindingsProbe", "ok", not anySucceeded)
    world:set(entity, "PhysicsBindingsProbe", "linear", world:linear_velocity(entity))
    world:set(entity, "PhysicsBindingsProbe", "angular", world:angular_velocity(entity))
end

return T
)LUA";
}

namespace
{
    constexpr std::string_view kLaunchOnStart = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    world:set_linear_velocity(entity, { x = 6.0, y = 0.0, z = 0.0 })
end

return T
)LUA";

    constexpr std::string_view kLaunchOnFixedUpdate = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnFixedUpdate(self, world, entity, dt)
    world:set_linear_velocity(entity, { x = 6.0, y = 0.0, z = 0.0 })
end

return T
)LUA";
}

TEST_CASE("a velocity set in OnStart takes effect on the next step", "[script][physics]")
{
    Fixture f(kLaunchOnStart);
    // the body is already there when OnStart runs
    REQUIRE(f.Body().IsValid());

    f.Start();
    f.Step();

    const std::optional<mir::WorldPosition> at = f.Physics().BodyPosition(f.Body());
    REQUIRE(at.has_value());
    CHECK(at->Meters().x > 0.05);
}

TEST_CASE("OnFixedUpdate runs before the step, so what it sets acts on that step", "[script][physics]")
{
    Fixture f(kLaunchOnFixedUpdate);
    f.world.AddComponent<mir::ScriptRef>(f.entity, mir::ScriptRef{});
    mir::ScriptRef *ref = f.world.GetComponent<mir::ScriptRef>(f.entity);
    ref->instanceRef = f.instance;
    ref->started = true;

    f.Step();

    // one step at 6 m/s is 0.1 m; had the callback run after the step it would still be at rest
    const std::optional<mir::WorldPosition> at = f.Physics().BodyPosition(f.Body());
    REQUIRE(at.has_value());
    CHECK(at->Meters().x > 0.05);
}

namespace
{
    constexpr std::string_view kEditFields = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local ok = true
    ok = ok and world:set_friction(entity, 0.7)
    ok = ok and world:set_half_extent(entity, { x = 1.0, y = 2.0, z = 3.0 })
    ok = ok and world:set_offset(entity, { x = 0.0, y = 4.0, z = 0.0 })
    ok = ok and world:set_rotation_offset(entity, { x = 0.0, y = 0.0, z = 0.0, w = 1.0 })
    ok = ok and world:set_is_trigger(entity, true)
    ok = ok and world:set_detect_stay(entity, true)
    ok = ok and world:set_motion(entity, PhysicsMotion.Kinematic)
    ok = ok and world:set_shape(entity, PhysicsShape.Capsule)
    ok = ok and world:set_axis(entity, PhysicsAxis.Z)
    ok = ok and world:set_mass(entity, 25.0)
    ok = ok and world:set_gravity_factor(entity, 0.5)
    ok = ok and world:set_restitution(entity, 0.3)
    ok = ok and world:set_allow_sleeping(entity, false)
    ok = ok and world:set_half_height(entity, 2.5)
    ok = ok and world:set_radius(entity, 0.75)
    ok = ok and world:set_interpolate(entity, true)

    local read = math.abs(world:friction(entity) - 0.7) < 1e-6
        and world:half_extent(entity).y == 2.0
        and world:offset(entity).y == 4.0
        and world:is_trigger(entity) == true
        and world:motion(entity) == 2
        and world:shape(entity) == 2
        and world:axis(entity) == 2
    world:set(entity, "PhysicsBindingsProbe", "ok", ok and read)
end

return T
)LUA";

    constexpr std::string_view kTeleport = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local moved = world:teleport(entity, { x = 20.0, y = 5.0, z = 0.0 })
    world:set(entity, "PhysicsBindingsProbe", "ok", moved == true)
end

return T
)LUA";

    constexpr std::string_view kTeleportNothing = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local moved = world:teleport(entity, { x = 20.0, y = 5.0, z = 0.0 })
    world:set(entity, "PhysicsBindingsProbe", "ok", moved == false)
end

return T
)LUA";

    constexpr std::string_view kSleepControl = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    world:wake(entity)
    world:set(entity, "PhysicsBindingsProbe", "ok", world:is_sleeping(entity) == false)
end

return T
)LUA";
}

namespace
{
    constexpr std::string_view kFreezeInFixedUpdate = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    world:set_linear_velocity(entity, { x = 6.0, y = 0.0, z = 0.0 })
end

function T.OnFixedUpdate(self, world, entity, dt)
    world:set_motion(entity, PhysicsMotion.Static)
end

return T
)LUA";
}

TEST_CASE("a RigidBody edit made in OnFixedUpdate reaches the body in the same step", "[script][physics][fields]")
{
    Fixture f(kFreezeInFixedUpdate);
    f.world.AddComponent<mir::ScriptRef>(f.entity, mir::ScriptRef{});
    mir::ScriptRef *ref = f.world.GetComponent<mir::ScriptRef>(f.entity);
    ref->instanceRef = f.instance;
    ref->started = true;
    f.Start();

    const mir::BodyHandle before = f.Body();
    f.Step();

    // the body was moving at 6 m/s, and the edit made it Static before the step moved it
    CHECK(f.Physics().BodyPosition(f.Body())->Meters().x == Catch::Approx(0.0).margin(1e-6));
    CHECK(f.Body() == before);
}

namespace
{
    // The caster is the fixture's box at the origin; the floor below it is added by the test.
    constexpr std::string_view kCastSelf = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

local function near(a, b) return math.abs(a - b) < 0.02 end

function T.OnStart(self, world, entity)
    local from, down = { x = 0.0, y = 3.0, z = 0.0 }, { x = 0.0, y = -1.0, z = 0.0 }
    local plain = world:raycast(from, down, 20.0)
    local self_hit = world:raycast(from, down, 20.0, { ignoreSelf = false })
    local excluded = world:raycast(from, down, 20.0, { ignoreSelf = false, exclude = { entity } })
    local ok = plain.hit and near(plain.distance, 5.5)
        and self_hit.hit and near(self_hit.distance, 2.5)
        and excluded.hit and near(excluded.distance, 5.5)
    world:set(entity, "PhysicsBindingsProbe", "ok", ok)
end

return T
)LUA";

    constexpr std::string_view kCastFilters = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

local function near(a, b) return math.abs(a - b) < 0.02 end

function T.OnStart(self, world, entity)
    local from, down = { x = 0.0, y = 3.0, z = 0.0 }, { x = 0.0, y = -1.0, z = 0.0 }
    local byName = world:raycast(from, down, 20.0, { layers = { "Floor" } })
    local byIndex = world:raycast(from, down, 20.0, { layers = { 2 } })
    local wrongLayer = world:raycast(from, down, 20.0, { layers = { "Default" } })
    local isStatic = world:raycast(from, down, 20.0, { motion = { PhysicsMotion.Static } })
    local isKinematic = world:raycast(from, down, 20.0, { motion = { PhysicsMotion.Kinematic } })
    local typo = world:raycast(from, down, 20.0, { layer = { "Floor" } })
    local nobody = world:raycast(from, down, 20.0, { layers = { "Nobody" } })
    local ok = byName.hit and near(byName.distance, 5.5)
        and byIndex.hit and near(byIndex.distance, 5.5)
        and not wrongLayer.hit
        and isStatic.hit and near(isStatic.distance, 5.5)
        and not isKinematic.hit
        and not typo.hit
        and not nobody.hit
    world:set(entity, "PhysicsBindingsProbe", "ok", ok)
end

return T
)LUA";

    constexpr std::string_view kCastTriggers = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local from, down = { x = 0.0, y = 3.0, z = 0.0 }, { x = 0.0, y = -1.0, z = 0.0 }
    local skipped = world:raycast(from, down, 20.0)
    local asked = world:raycast(from, down, 20.0, { hitTriggers = true })
    world:set(entity, "PhysicsBindingsProbe", "ok", not skipped.hit and asked.hit)
end

return T
)LUA";

    // A floor under the caster, 5.5 meters below the cast's start.
    void AddFloor(Fixture &f, int layer = 0, bool trigger = false)
    {
        mir::RigidBody floor{ .motion = mir::PhysicsMotion::Static, .shape = mir::PhysicsShape::Box,
            .halfExtent = glm::vec3(5.0f, 0.5f, 5.0f) };
        floor.layer = layer;
        floor.isTrigger = trigger;
        const mir::Entity entity = f.world.CreateEntity();
        mir::AddTransform(f.world, entity, mir::Transform{ mir::WorldVec3(0.0, -3.0, 0.0) });
        f.world.AddComponent<mir::RigidBody>(entity, floor);
    }
}

TEST_CASE("a cast does not hit its caster unless it says so, and can exclude entities", "[script][physics][raycast]")
{
    Fixture f(kCastSelf);
    AddFloor(f);
    f.Start();
    CHECK(ReadProbeField<bool>(f.world, f.entity, "ok"));
}

TEST_CASE("a cast filters by layer name or index and by motion type, and refuses an unknown option", "[script][physics][raycast]")
{
    Fixture f(kCastFilters);
    mir::PhysicsLayerTable table;
    table.names[2] = "Floor";
    f.Physics().SetLayerTable(table);
    AddFloor(f, 2);
    f.Start();
    CHECK(ReadProbeField<bool>(f.world, f.entity, "ok"));
}

TEST_CASE("a cast skips triggers unless hitTriggers is set", "[script][physics][raycast]")
{
    Fixture f(kCastTriggers);
    AddFloor(f, 0, true);
    f.Start();
    CHECK(ReadProbeField<bool>(f.world, f.entity, "ok"));
}

namespace
{
    constexpr std::string_view kLayers = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local ok = true
    ok = ok and world:layer_index("Enemy") == 3
    ok = ok and world:layer_index("Nobody") == nil
    ok = ok and world:set_layer(entity, "Enemy")
    ok = ok and world:layer(entity) == 3
    ok = ok and world:set_layer(entity, 5)
    ok = ok and world:layer(entity) == 5
    -- a name that does not exist, an index out of range and a wrong type are refused and change nothing
    ok = ok and world:set_layer(entity, "Nobody") == false
    ok = ok and world:set_layer(entity, 40) == false
    ok = ok and world:set_layer(entity, 1.5) == false
    ok = ok and world:set_layer(entity, true) == false
    ok = ok and world:layer(entity) == 5
    world:set(entity, "PhysicsBindingsProbe", "ok", ok)
end

return T
)LUA";
}

TEST_CASE("layers are set by name or index and looked up by name", "[script][physics][layers]")
{
    Fixture f(kLayers);
    mir::PhysicsLayerTable table;
    table.names[3] = "Enemy";
    f.Physics().SetLayerTable(table);
    f.Start();

    CHECK(ReadProbeField<bool>(f.world, f.entity, "ok"));
    CHECK(f.world.GetComponent<mir::RigidBody>(f.entity)->layer == 5);
}

TEST_CASE("every RigidBody field can be read and written from Lua", "[script][physics][fields]")
{
    Fixture f(kEditFields);
    f.Start();

    CHECK(ReadProbeField<bool>(f.world, f.entity, "ok"));

    // and the component holds what the script wrote, the same field the editor edits
    const mir::RigidBody &rigidBody = *f.world.GetComponent<mir::RigidBody>(f.entity);
    CHECK(rigidBody.friction == Catch::Approx(0.7f));
    CHECK(rigidBody.halfExtent == glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(rigidBody.offset == glm::vec3(0.0f, 4.0f, 0.0f));
    CHECK(rigidBody.isTrigger);
    CHECK(rigidBody.detectStay);
    CHECK(rigidBody.motion == mir::PhysicsMotion::Kinematic);
    CHECK(rigidBody.shape == mir::PhysicsShape::Capsule);
    CHECK(rigidBody.axis == mir::PhysicsAxis::Z);
    CHECK(rigidBody.mass == 25.0f);
    CHECK(rigidBody.gravityFactor == 0.5f);
    CHECK(rigidBody.restitution == Catch::Approx(0.3f));
    CHECK_FALSE(rigidBody.allowSleeping);
    CHECK(rigidBody.halfHeight == 2.5f);
    CHECK(rigidBody.radius == 0.75f);
    CHECK(rigidBody.interpolate);
}

TEST_CASE("the RigidBody getters answer nil and the setters false for an entity without one", "[script][physics][fields]")
{
    Fixture f(kEditFields, false);
    f.Start();

    // the first setter fails, so the script reports it
    CHECK_FALSE(ReadProbeField<bool>(f.world, f.entity, "ok"));
}

TEST_CASE("world:teleport puts the Transform and the body at a world pose", "[script][physics][teleport]")
{
    Fixture f(kTeleport);
    f.Start();
    CHECK(ReadProbeField<bool>(f.world, f.entity, "ok"));

    CHECK(f.world.GetComponent<mir::Transform>(f.entity)->Position() == mir::WorldVec3(20.0, 5.0, 0.0));
    const std::optional<mir::WorldPosition> at = f.Physics().BodyPosition(f.Body());
    REQUIRE(at.has_value());
    CHECK(at->Meters() == mir::WorldVec3(20.0, 5.0, 0.0));

    // a step afterwards does not undo it
    f.Step();
    CHECK(f.world.GetComponent<mir::Transform>(f.entity)->Position().x == Catch::Approx(20.0).margin(1e-3));
}

TEST_CASE("world:teleport on an entity with no body is false", "[script][physics][teleport]")
{
    Fixture f(kTeleportNothing, false);
    f.Start();
    CHECK(ReadProbeField<bool>(f.world, f.entity, "ok"));
}

TEST_CASE("world:wake and world:is_sleeping drive the body's sleep", "[script][physics][sleep]")
{
    Fixture f(kSleepControl);
    f.Start();
    CHECK(ReadProbeField<bool>(f.world, f.entity, "ok"));
}

TEST_CASE("world:add_force with no offset pushes the center of mass", "[script][physics]")
{
    Fixture fixture(kPushForward);

    fixture.Frames(30);

    const glm::vec3 linear = fixture.Linear();
    CHECK(linear.z > 0.5f);
    CHECK(std::abs(linear.x) < kStill);
    CHECK(std::abs(linear.y) < kStill);

    // A force at the center of mass is purely linear - no torque falls out.
    CHECK(glm::length(fixture.Angular()) < kStill);
}

TEST_CASE("world:add_force at an offset also torques the body", "[script][physics]")
{
    Fixture fixture(kPushForwardAtOffset);

    fixture.Frames(10);

    // +Z pushed at +X from the center of mass: r x F points down -Y.
    const glm::vec3 angular = fixture.Angular();
    CHECK(angular.y < -0.05f);
    CHECK(std::abs(angular.x) < kStill);
    CHECK(std::abs(angular.z) < kStill);

    // ...and the body still accelerates the way the plain force would.
    CHECK(fixture.Linear().z > 0.01f);
}

TEST_CASE("world:add_torque spins the body in place", "[script][physics]")
{
    Fixture fixture(kTwist);

    fixture.Frames(10);

    CHECK(fixture.Angular().y > 0.05f);
    CHECK(glm::length(fixture.Linear()) < kStill);
}

TEST_CASE("the velocity bindings read the live body, not a stale copy", "[script][physics]")
{
    Fixture fixture(kPushAndRecord);

    fixture.Frames(10);

    // One more script call with no step after it, so the probe and the physics
    // world are being asked about the very same state.
    fixture.Script();
    const glm::vec3 linear = ReadProbeField<glm::vec3>(fixture.world, fixture.entity, "linear");
    const glm::vec3 angular = ReadProbeField<glm::vec3>(fixture.world, fixture.entity, "angular");

    CHECK(linear == fixture.Linear());
    CHECK(angular == fixture.Angular());
    CHECK(linear.z > 0.01f);
    CHECK(angular.y < -0.01f);

    // Another frame of the same push has to show up in the next read.
    fixture.Step();
    fixture.Script();
    CHECK(ReadProbeField<glm::vec3>(fixture.world, fixture.entity, "linear").z > linear.z);
}

TEST_CASE("world:set_/add_ velocity write through to the body", "[script][physics]")
{
    Fixture fixture(kWriteVelocity);

    fixture.Start();

    const glm::vec3 linear = fixture.Linear();
    CHECK(std::abs(linear.x - 2.0f) < kStill); // set 1, then added 1
    CHECK(std::abs(linear.y - 2.0f) < kStill);
    CHECK(std::abs(linear.z - 3.0f) < kStill);

    const glm::vec3 angular = fixture.Angular();
    CHECK(std::abs(angular.y - 5.0f) < kStill); // set 4, then added 1
    CHECK(std::abs(angular.x) < kStill);
    CHECK(std::abs(angular.z) < kStill);
}

TEST_CASE("world:add_impulse changes velocity without waiting for a step", "[script][physics]")
{
    Fixture fixture(kImpulses);

    fixture.Start();

    CHECK(fixture.Linear().z > 0.0f);
    CHECK(fixture.Angular().y > 0.0f);
}

TEST_CASE("world:add_impulse at an offset spins the body too", "[script][physics]")
{
    Fixture fixture(kOffsetImpulse);

    fixture.Start();

    CHECK(fixture.Linear().z > 0.0f);
    CHECK(fixture.Angular().y < 0.0f);
}

TEST_CASE("the physics bindings are inert on an entity with no RigidBody", "[script][physics]")
{
    Fixture fixture(kProbeEverything, /*withRigidBody=*/false);

    fixture.Start();

    // True only if the script ran all the way through and every writer refused.
    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "ok"));
    CHECK(ReadProbeField<glm::vec3>(fixture.world, fixture.entity, "linear") == glm::vec3(0.0f));
    CHECK(ReadProbeField<glm::vec3>(fixture.world, fixture.entity, "angular") == glm::vec3(0.0f));
}

namespace
{
    constexpr uint32_t kFieldSide = 33;
    constexpr double kFieldSpacing = 4.0;
    constexpr double kPlaneX = -0.08, kPlaneZ = -0.03;
    double Plane(double x, double z) { return 50.0 + kPlaneX * x + kPlaneZ * z; }
    // Jolt's height quantization plus the float hit fraction.
    constexpr double kFieldTolerance = 0.0025;

    const glm::dvec3 kFar{ 150000.5, -20.0, -75000.25 };

    const mir::ComponentOps &RaycastProbeOps()
    {
        static const mir::RuntimeFieldDecl fields[] = {
            { "ran", mir::FieldKind::Bool },{ "hit", mir::FieldKind::Bool },
            { "bare", mir::FieldKind::Bool },{ "point", mir::FieldKind::WorldPosition },
            { "normal", mir::FieldKind::Vec3 },{ "distance", mir::FieldKind::Float },
            { "hasEntity", mir::FieldKind::Bool },{ "entity", mir::FieldKind::EntityRef },
        };
        return mir::ComponentRegistry::Instance().RegisterRuntime("RaycastProbe", fields);
    }

    // A std::format string: {} takes the raycast arguments, {{}} is a Lua table.
    constexpr std::string_view kRaycastScript = R"LUA(
local T = {{}}

function T.NewInstanceData()
    return {{}}
end

function T.OnStart(self, world, entity)
    local r = world:raycast({})
    world:set(entity, "RaycastProbe", "hit", r.hit)
    world:set(entity, "RaycastProbe", "bare", r.point == nil and r.normal == nil and r.distance == nil
        and r.entity == nil)
    if r.hit then
        world:set(entity, "RaycastProbe", "point", r.point)
        world:set(entity, "RaycastProbe", "normal", r.normal)
        world:set(entity, "RaycastProbe", "distance", r.distance)
    end
    world:set(entity, "RaycastProbe", "hasEntity", r.entity ~= nil)
    if r.entity ~= nil then
        world:set(entity, "RaycastProbe", "entity", r.entity)
    end
    world:set(entity, "RaycastProbe", "ran", true)
end

return T
)LUA";

    struct RaycastResult
    {
        bool hit = false;
        bool bare = false;
        mir::WorldVec3 point{};
        glm::vec3 normal{};
        float distance = 0.0f;
        bool hasEntity = false;
        mir::Entity entity{};
    };

    std::string Table(const glm::dvec3 &v) { return std::format("{{ x = {}, y = {}, z = {} }}", v.x, v.y, v.z); }

    struct RaycastFixture
    {
        mir::World world;
        mir::CommandBuffer commands;
        mir::SystemScheduler scheduler;
        mir::SystemContext context{ world, commands, kStep, 0.0, 0 };
        // what PhysicsModule sets, so an added RigidBody gets its body
        mir::Registration hooks;
        mir::ScriptHost host;
        const mir::Registration bindings = mir::AddBindings(host, &mir::RegisterPhysicsBindings);
        std::vector<float> samples;
        int casts = 0;

        RaycastFixture()
        {
            // No gravity, so a dynamic body stays where the ray expects it.
            world.EmplaceResource<mir::PhysicsWorld>(mir::PhysicsWorldDesc{ .mGravity = glm::vec3(0.0f) });
            world.Resource<mir::PhysicsWorld>().SetSimulationLive(true);
            hooks = world.SetComponentHooks<mir::RigidBody>(
                { .onAdd = &mir::PhysicsSystem::OnRigidBodyAdded, .onRemove = &mir::PhysicsSystem::OnRigidBodyRemoved });
            world.EmplaceResource<mir::EngineModeState>();
            mir::SetEngineMode(world, mir::EngineMode::Play);
            mir::SetPlayState(world, mir::PlayState::Running);

            scheduler.AddSystem<mir::PhysicsSystem>(mir::SystemPhase::FixedUpdate, mir::SystemOrder{mir::SystemOrder::kNormal});
            scheduler.Start(context);

            for (uint32_t k = 0; k < kFieldSide; ++k)
                for (uint32_t i = 0; i < kFieldSide; ++i)
                    samples.push_back(static_cast<float>(Plane(i * kFieldSpacing, k * kFieldSpacing)));
        }

        ~RaycastFixture()
        {
            if (scheduler.Started())
                scheduler.Stop(context);
        }

        mir::PhysicsWorld &Physics() { return world.Resource<mir::PhysicsWorld>(); }

        mir::BodyHandle AddField(const glm::dvec3 &origin, mir::Entity tag)
        {
            const std::optional<mir::WorldPosition> at = mir::WorldPosition::FromDoubleMeters(origin);
            REQUIRE(at);
            mir::HeightFieldDesc desc;
            desc.mSamples = samples;
            desc.mCountX = kFieldSide;
            desc.mCountZ = kFieldSide;
            desc.mSpacing = static_cast<float>(kFieldSpacing);
            desc.mOrigin = *at;
            const mir::BodyHandle body = Physics().CreateStaticHeightField(desc);
            REQUIRE(body.IsValid());
            Physics().SetBodyEntity(body, tag);
            return body;
        }

        RaycastResult Cast(std::string_view args)
        {
            const mir::Entity probe = world.CreateEntity();
            RaycastProbeOps().AddDefault(world, probe);

            const std::string name = std::format("raycast_probe_{}", casts++);
            REQUIRE(host.LoadScriptSource(name, std::vformat(kRaycastScript, std::make_format_args(args))));
            const int32_t instance = host.CreateInstance(name);
            REQUIRE(instance != -1);
            host.CallOnStart(world, commands, probe, instance);
            commands.Flush(world);
            host.DestroyInstance(instance);

            const mir::ComponentOps &ops = RaycastProbeOps();
            const void *component = ops.GetComponent(world, probe);
            const auto read = [&](std::string_view field, void *out) { ops.FindField(field)->Read(component, out); };
            bool ran = false;
            read("ran", &ran);
            REQUIRE(ran);

            RaycastResult result;
            read("hit", &result.hit);
            read("bare", &result.bare);
            read("point", &result.point);
            read("normal", &result.normal);
            read("distance", &result.distance);
            read("hasEntity", &result.hasEntity);
            read("entity", &result.entity);
            return result;
        }
    };

    void CheckFieldHit(const RaycastResult &hit, const glm::dvec3 &fieldOrigin, double x, double z, double above)
    {
        REQUIRE(hit.hit);
        const double height = Plane(x, z);
        const glm::dvec3 point(hit.point);
        // Straight down, so x and z come back bit for bit.
        CHECK(point.x == fieldOrigin.x + x);
        CHECK(point.z == fieldOrigin.z + z);
        CHECK(std::abs(point.y - (fieldOrigin.y + height)) <= kFieldTolerance);
        CHECK(std::abs(hit.distance - (above - height)) <= kFieldTolerance);
        const glm::vec3 up = glm::normalize(glm::vec3(static_cast<float>(-kPlaneX), 1.0f, static_cast<float>(-kPlaneZ)));
        CHECK(glm::length(hit.normal - up) <= 1.0e-3f);
    }
}

TEST_CASE("world:raycast hits a terrain height field and names its entity", "[script][physics]")
{
    RaycastFixture fixture;
    const mir::Entity terrain = fixture.world.CreateEntity();
    fixture.AddField(glm::dvec3(0.0), terrain);

    const RaycastResult hit = fixture.Cast(Table({ 37.5, 100.0, 61.25 }) + ", { x = 0, y = -3, z = 0 }");
    CheckFieldHit(hit, glm::dvec3(0.0), 37.5, 61.25, 100.0);
    CHECK(hit.hasEntity);
    CHECK(hit.entity == terrain);
}

TEST_CASE("world:raycast far from the origin is as precise as at the origin", "[script][physics][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    RaycastFixture fixture;
    const mir::Entity terrain = fixture.world.CreateEntity();
    fixture.AddField(kFar, terrain);

    const RaycastResult hit = fixture.Cast(Table(kFar + glm::dvec3(37.5, 100.0, 61.25)) + ", { x = 0, y = -1, z = 0 }");
    CheckFieldHit(hit, kFar, 37.5, 61.25, 100.0);
    CHECK(hit.entity == terrain);
}

TEST_CASE("world:raycast hits a dynamic RigidBody and names its entity", "[script][physics]")
{
    RaycastFixture fixture;
    const mir::Entity box = fixture.world.CreateEntity();
    mir::AddTransform(fixture.world, box, mir::Transform{ mir::WorldVec3(2.0, 5.0, -1.0) });
    fixture.world.AddComponent<mir::RigidBody>(box, kDynamicBox);
    fixture.scheduler.Update(fixture.context, 1, kStep);

    const RaycastResult hit = fixture.Cast("{ x = 2, y = 10, z = -1 }, { x = 0, y = -1, z = 0 }, 20");
    REQUIRE(hit.hit);
    CHECK(std::abs(glm::dvec3(hit.point).y - 5.5) <= 1.0e-4);
    CHECK(std::abs(hit.distance - 4.5f) <= 1.0e-4f);
    CHECK(glm::length(hit.normal - glm::vec3(0.0f, 1.0f, 0.0f)) <= 1.0e-4f);
    CHECK(hit.hasEntity);
    CHECK(hit.entity == box);

    const RaycastResult inside = fixture.Cast("{ x = 2, y = 5, z = -1 }, { x = 0, y = -1, z = 0 }");
    REQUIRE(inside.hit);
    CHECK(inside.distance == 0.0f);
    CHECK(inside.entity == box);
}

TEST_CASE("world:raycast reports a clean miss", "[script][physics]")
{
    RaycastFixture fixture;
    const RaycastResult empty = fixture.Cast("{ x = 0, y = 10, z = 0 }, { x = 0, y = -1, z = 0 }");
    CHECK_FALSE(empty.hit);
    CHECK(empty.bare);

    fixture.AddField(glm::dvec3(0.0), fixture.world.CreateEntity());
    const RaycastResult away = fixture.Cast("{ x = 10, y = 100, z = 10 }, { x = 0, y = 1, z = 0 }");
    CHECK_FALSE(away.hit);
    CHECK(away.bare);
    const RaycastResult beside = fixture.Cast("{ x = -10, y = 100, z = 10 }, { x = 0, y = -1, z = 0 }");
    CHECK_FALSE(beside.hit);
}

TEST_CASE("world:raycast stops at maxDistance or its default", "[script][physics]")
{
    RaycastFixture fixture;
    fixture.AddField(glm::dvec3(0.0), fixture.world.CreateEntity());

    CHECK_FALSE(fixture.Cast("{ x = 37.5, y = 100, z = 61.25 }, { x = 0, y = -1, z = 0 }, 10").hit);
    CheckFieldHit(fixture.Cast("{ x = 37.5, y = 100, z = 61.25 }, { x = 0, y = -1, z = 0 }"), glm::dvec3(0.0), 37.5,
        61.25, 100.0);
    CheckFieldHit(fixture.Cast("{ x = 37.5, y = 100, z = 61.25 }, { x = 0, y = -1, z = 0 }, nil"), glm::dvec3(0.0),
        37.5, 61.25, 100.0);

    CHECK_FALSE(fixture.Cast("{ x = 37.5, y = 1100, z = 61.25 }, { x = 0, y = -1, z = 0 }").hit);
    CHECK(fixture.Cast("{ x = 37.5, y = 1100, z = 61.25 }, { x = 0, y = -1, z = 0 }, 2000").hit);
}

TEST_CASE("world:raycast refuses bad arguments with a miss, never an assert", "[script][physics]")
{
    RaycastFixture fixture;
    fixture.AddField(glm::dvec3(0.0), fixture.world.CreateEntity());

    const std::string down = "{ x = 0, y = -1, z = 0 }";
    const std::string above = "{ x = 37.5, y = 100, z = 61.25 }";
    for (const std::string &args :{
             "42, " + down,
             "nil, " + down,
             "{ x = 1/0, y = 100, z = 10 }, " + down,
             "{ x = 10, y = 0/0, z = 10 }, " + down,
             above + ", 'down'",
             above + ", { x = 0, y = 0, z = 0 }",
             above + ", { x = 0, y = 0/0, z = 0 }",
             above + ", { x = 0, y = -1/0, z = 0 }",
             above + ", " + down + ", 0",
             above + ", " + down + ", -5",
             above + ", " + down + ", 0/0",
             above + ", " + down + ", 1/0",
             above + ", " + down + ", 'far'",
        })
    {
        INFO(args);
        const RaycastResult result = fixture.Cast(args);
        CHECK_FALSE(result.hit);
        CHECK(result.bare);
    }
}

TEST_CASE("world:raycast gives no entity for a destroyed one, still a hit", "[script][physics]")
{
    RaycastFixture fixture;
    const mir::Entity terrain = fixture.world.CreateEntity();
    fixture.AddField(glm::dvec3(0.0), terrain);
    fixture.world.DestroyEntity(terrain);

    const RaycastResult hit = fixture.Cast("{ x = 37.5, y = 100, z = 61.25 }, { x = 0, y = -1, z = 0 }");
    CHECK(hit.hit);
    CHECK_FALSE(hit.hasEntity);
}
