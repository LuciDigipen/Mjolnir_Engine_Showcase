/**
 * @file FlightBindings.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight_script/FlightBindings.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight_script/FlightBindings.h>

#include <assets/AssetBlob.h>
#include <core/ecs/CommandBuffer.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/ModuleContext.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <flight/AircraftConfig.h>
#include <flight/AircraftRuntime.h>
#include <flight/FlightModule.h>
#include <flight/components/Aircraft.h>
#include <physics/ComponentRegistration.h>
#include <physics/PhysicsSystem.h>
#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>
#include <script/BindingApi.h>
#include <script/ComponentRegistration.h>
#include <script/ScriptFixedSystem.h>
#include <script/ScriptHost.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr float kStep = 1.0f / 60.0f;
    constexpr std::string_view kConfigPath = "script.aircraft.json";

    constexpr std::string_view kAircraftDocument = R"({
        "version": 1,
        "mass": 1000.0,
        "inertia": { "diagonal": [2000.0, 3500.0, 2500.0] },
        "centerOfMass": [0.0, 0.0, 0.0],
        "reference": { "area": 16.0, "span": 11.0, "chord": 1.5 },
        "camera": { "eye": [0.8, 0.0, -0.4], "chaseDistance": 12.0, "chaseHeight": 3.0 },
        "throttleRate": 0.5,
        "actuators": {
            "elevator": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 600.0, "delay": 0.0 },
            "aileron": { "maxDeflectionDeg": 20.0, "rateDegPerSec": 600.0, "delay": 0.0 },
            "rudder": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 600.0, "delay": 0.0 },
            "flaps": { "detentsDeg": [0.0, 10.0], "rateDegPerSec": 5.0 },
            "trim": { "stepDeg": 0.5, "maxDeg": 10.0 }
        },
        "engines": [
            { "position": [0.0, 0.0, 0.0], "direction": [1.0, 0.0, 0.0], "idleRpm": 700.0, "maxRpm": 2700.0,
              "rpmTau": 0.8, "staticThrust": [[0.0, 0.0], [1.0, 3200.0]], "speedFactor": [[0.0, 1.0]] }
        ],
        "gear": [
            { "name": "wheel", "mount": [0.0, 0.0, 0.0], "restLength": 0.6, "stiffness": 10000.0,
              "damping": 1000.0, "rollingFriction": 0.02, "sideFriction": 0.5, "brake": 0.0, "maxSteerDeg": 0.0 }
        ]
    })";

    struct RawConfig
    {
        explicit RawConfig(std::string_view json)
            : raw(mir::BuildAssetBlob(mir::kRawAssetTypeTag, mir::kRawAssetContentVersion,
                  std::span<const std::byte>{reinterpret_cast<const std::byte *>(json.data()), json.size()})),
            view(mir::ParseAssetBlob(raw))
        {
        }

        std::vector<std::byte> raw;
        std::optional<mir::AssetBlobView> view;
    };

    void SetConfig(mir::Aircraft &aircraft, std::string_view path)
    {
        std::memset(aircraft.config, 0, sizeof(aircraft.config));
        if (!path.empty())
            std::memcpy(aircraft.config, path.data(), std::min(path.size(), sizeof(aircraft.config) - 1));
    }

    const mir::ComponentOps &ProbeOps()
    {
        static const mir::RuntimeFieldDecl fields[] = {
            { "ok", mir::FieldKind::Bool },
            { "airspeed", mir::FieldKind::Float },
            { "heading", mir::FieldKind::Float },
            { "altitude", mir::FieldKind::Float },
            { "rpm1", mir::FieldKind::Float },
            { "engine_count", mir::FieldKind::Int },
            { "gear_count", mir::FieldKind::Int },
            { "force", mir::FieldKind::Vec3 },
            { "pitch", mir::FieldKind::Float },
            { "trim", mir::FieldKind::Float },
            { "throttle", mir::FieldKind::Float },
            { "brake", mir::FieldKind::Bool },
            { "detent", mir::FieldKind::Int },
            { "count", mir::FieldKind::Int },
            { "sink", mir::FieldKind::Float },
        };
        return mir::ComponentRegistry::Instance().RegisterRuntime("FlightScriptProbe", fields);
    }

    template <typename T>
    T ReadProbeField(mir::World &world, mir::Entity entity, std::string_view field)
    {
        const mir::ComponentOps &probe = ProbeOps();
        T value{};
        probe.FindField(field)->Read(probe.GetComponent(world, entity), &value);
        return value;
    }

    constexpr mir::RigidBody kDynamicBox{
        .motion = mir::PhysicsMotion::Dynamic,
        .shape = mir::PhysicsShape::Box,
        .halfExtent = glm::vec3(1.5f, 0.25f, 2.0f),
        .mass = 1000.0f,
        .allowSleeping = false,
    };

    struct ConfigSource
    {
        const mir::AssetBlobView *view = nullptr;

        const mir::AssetBlobView *operator()(std::string_view path, std::string &) const
        {
            return path == kConfigPath ? view : nullptr;
        }
    };

    struct Fixture
    {
        mir::World world;
        mir::CommandBuffer commands;
        mir::SystemScheduler scheduler;
        mir::SystemContext context{ world, commands, kStep, 0.0, 0 };
        mir::ScriptHost host;
        const mir::Registration bindings = mir::AddBindings(host, &mir::RegisterFlightBindings);
        mir::Registration hooks;
        std::optional<mir::FlightModule> flight;
        mir::Entity entity{};
        int32_t instance = -1;

        Fixture(const RawConfig &config, std::string_view source, bool withAircraft = true, double height = 0.0)
        {
            mir::RegisterScriptComponents();
            mir::RegisterPhysicsComponents();
            world.EmplaceResource<mir::PhysicsWorld>(mir::PhysicsWorldDesc{ .mGravity = glm::vec3(0.0f, -9.81f, 0.0f) });
            world.Resource<mir::PhysicsWorld>().SetSimulationLive(true);
            hooks = world.SetComponentHooks<mir::RigidBody>(
                { .onAdd = &mir::PhysicsSystem::OnRigidBodyAdded, .onRemove = &mir::PhysicsSystem::OnRigidBodyRemoved });
            world.EmplaceResource<mir::EngineModeState>();
            mir::SetEngineMode(world, mir::EngineMode::Play);
            mir::SetPlayState(world, mir::PlayState::Running);

            scheduler.AddSystem<mir::PhysicsSystem>(mir::SystemPhase::FixedUpdate,
                mir::SystemOrder{ mir::SystemOrder::kNormal });
            flight.emplace(mir::ModuleContext{ world, scheduler }, ConfigSource{ &*config.view });
            scheduler.AddSystem<mir::ScriptFixedSystem>(mir::SystemPhase::FixedUpdate,
                mir::SystemOrder{ mir::SystemOrder::kEarly }, host);
            scheduler.Start(context);

            entity = world.CreateEntity();
            mir::AddTransform(world, entity, mir::Transform{ mir::WorldVec3(0.0, height, 0.0) });
            world.AddComponent<mir::RigidBody>(entity, kDynamicBox);
            if (withAircraft)
            {
                mir::Aircraft aircraft;
                SetConfig(aircraft, kConfigPath);
                world.AddComponent<mir::Aircraft>(entity, aircraft);
            }
            ProbeOps().AddDefault(world, entity);

            REQUIRE(host.LoadScriptSource("flight_probe", source));
            instance = host.CreateInstance("flight_probe");
            REQUIRE(instance != -1);

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

        mir::Aircraft &Aircraft() { return *world.GetComponent<mir::Aircraft>(entity); }

        void Start()
        {
            host.CallOnStart(world, commands, entity, instance);
            commands.Flush(world);
        }

        void Script()
        {
            host.CallOnUpdate(world, commands, entity, instance, kStep);
            commands.Flush(world);
        }

        // The fixed-step callback, which a started script runs before the physics step.
        void Fixed()
        {
            host.CallOnFixedUpdate(world, commands, entity, instance, kStep);
            commands.Flush(world);
        }

        void Step() { scheduler.Update(context, 1, kStep); }

        void AddGround()
        {
            mir::BodyDesc desc;
            desc.mMotion = mir::PhysicsMotion::Static;
            desc.mShape = mir::PhysicsShape::Box;
            desc.mPosition = *mir::WorldPosition::FromMeters(mir::WorldVec3(0.0, -0.5, 0.0));
            desc.mHalfExtent = glm::vec3{ 200.0f, 0.5f, 200.0f };
            Physics().CreateBody(desc);
        }
    };

    constexpr std::string_view kReadTelemetry = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnUpdate(self, world, entity, dt)
    local t = world:aircraft_telemetry(entity)
    local ok = t ~= nil
        and t.true_airspeed ~= nil and t.indicated_airspeed ~= nil and t.altitude ~= nil
        and t.height_above_ground ~= nil and t.has_ground ~= nil and t.vertical_speed ~= nil
        and t.heading ~= nil and t.pitch ~= nil and t.roll ~= nil
        and t.angle_of_attack ~= nil and t.sideslip ~= nil and t.dynamic_pressure ~= nil
        and t.mach ~= nil and t.load_factor ~= nil and t.stall_margin ~= nil
        and t.stall_warning ~= nil and t.throttle ~= nil
        and t.engine_count == 1 and #t.rpm == 1
        and t.gear_count == 1 and #t.gear_compression == 1
        and t.weight_on_wheels == false
        and t.flaps == 0.0 and t.trim == 0.0
        and t.elevator == 0.0 and t.aileron == 0.0 and t.rudder == 0.0
        and t.aero_forces.force.x == 0.0 and t.aero_forces.moment.z == 0.0
        and t.propulsion_forces.force ~= nil and t.gear_forces.moment ~= nil
    world:set(entity, "FlightScriptProbe", "ok", ok)
    if t == nil then
        return
    end
    world:set(entity, "FlightScriptProbe", "airspeed", t.true_airspeed)
    world:set(entity, "FlightScriptProbe", "heading", t.heading)
    world:set(entity, "FlightScriptProbe", "altitude", t.altitude)
    world:set(entity, "FlightScriptProbe", "rpm1", t.rpm[1])
    world:set(entity, "FlightScriptProbe", "engine_count", t.engine_count)
    world:set(entity, "FlightScriptProbe", "gear_count", t.gear_count)
    world:set(entity, "FlightScriptProbe", "force", t.aero_forces.force)
end

return T
)LUA";

    constexpr std::string_view kWindOnce = R"LUA(
local T = {}

function T.NewInstanceData()
    return { steps = 0 }
end

function T.OnFixedUpdate(self, world, entity, dt)
    if self.steps == 0 then
        world:set(entity, "FlightScriptProbe", "ok",
            world:set_aircraft_wind(entity, { x = 0.0, y = 0.0, z = 10.0 }) == true)
    end
    self.steps = self.steps + 1
end

function T.OnUpdate(self, world, entity, dt)
    local t = world:aircraft_telemetry(entity)
    if t ~= nil then
        world:set(entity, "FlightScriptProbe", "airspeed", t.true_airspeed)
    end
end

return T
)LUA";

    constexpr std::string_view kReset = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local moved = world:reset_aircraft(entity, { x = 20.0, y = 5.0, z = 0.0 },
        { x = 0.0, y = 0.0, z = 0.0, w = 1.0 }, 30.0, 0.5)
    world:set(entity, "FlightScriptProbe", "ok", moved == true)
end

return T
)LUA";

    constexpr std::string_view kReadTouchdowns = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnUpdate(self, world, entity, dt)
    local events = world:aircraft_touchdowns(entity)
    local count = 0
    local ok = events ~= nil
    if ok then
        count = #events
        ok = count == 0 or events[1].gear == 1
        if count > 0 then
            world:set(entity, "FlightScriptProbe", "sink", events[1].sink_rate)
        end
    end
    world:set(entity, "FlightScriptProbe", "ok", ok)
    world:set(entity, "FlightScriptProbe", "count", count)
end

return T
)LUA";

    constexpr std::string_view kReadControls = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnUpdate(self, world, entity, dt)
    local c = world:aircraft_controls(entity)
    world:set(entity, "FlightScriptProbe", "ok", c ~= nil)
    if c == nil then
        return
    end
    world:set(entity, "FlightScriptProbe", "pitch", c.pitch)
    world:set(entity, "FlightScriptProbe", "trim", c.trim_deg)
    world:set(entity, "FlightScriptProbe", "throttle", c.throttle)
    world:set(entity, "FlightScriptProbe", "brake", c.brake)
    world:set(entity, "FlightScriptProbe", "detent", c.flaps_detent)
end

return T
)LUA";

    constexpr std::string_view kNoAircraft = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local any = world:aircraft_telemetry(entity) ~= nil
        or world:aircraft_touchdowns(entity) ~= nil
        or world:aircraft_controls(entity) ~= nil
        or world:set_aircraft_wind(entity, { x = 1.0, y = 0.0, z = 0.0 }) == true
        or world:reset_aircraft(entity, { x = 1.0, y = 0.0, z = 0.0 },
            { x = 0.0, y = 0.0, z = 0.0, w = 1.0 }, 10.0, 0.0) == true
    world:set(entity, "FlightScriptProbe", "ok", not any)
end

return T
)LUA";

    constexpr std::string_view kBadArguments = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local refused = world:set_aircraft_wind(entity, "not a vector") == false
        and world:set_aircraft_wind(entity, { x = 0.0 / 0.0, y = 0.0, z = 0.0 }) == false
        and world:reset_aircraft(entity, "not a position",
            { x = 0.0, y = 0.0, z = 0.0, w = 1.0 }, 10.0, 0.0) == false
        and world:reset_aircraft(entity, { x = 1.0, y = 0.0, z = 0.0 },
            { x = 0.0, y = 0.0, z = 0.0, w = 1.0 }, "fast", 0.0) == false
    world:set(entity, "FlightScriptProbe", "ok", refused)
end

return T
)LUA";
}

TEST_CASE("aircraft telemetry reports every field with degrees and 1-based arrays", "[script][flight]")
{
    const RawConfig config(kAircraftDocument);
    REQUIRE(config.view.has_value());
    Fixture fixture(config, kReadTelemetry, true, 500.0);

    REQUIRE(fixture.Physics().SetBodyLinearVelocity(fixture.Body(), glm::vec3{ 0.0f, 0.0f, -50.0f }));
    fixture.Step();
    fixture.Script();

    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "ok"));
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "airspeed") == Catch::Approx(50.0f).margin(0.5f));
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "heading") == Catch::Approx(0.0f).margin(0.5f));
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "altitude") == Catch::Approx(500.0f).margin(1.0f));
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "rpm1") == Catch::Approx(700.0f).margin(1.0f));
    CHECK(ReadProbeField<int32_t>(fixture.world, fixture.entity, "engine_count") == 1);
    CHECK(ReadProbeField<int32_t>(fixture.world, fixture.entity, "gear_count") == 1);
    CHECK(ReadProbeField<glm::vec3>(fixture.world, fixture.entity, "force") == glm::vec3{ 0.0f });
}

TEST_CASE("wind written in OnFixedUpdate acts on that step and is gone from the next", "[script][flight][wind]")
{
    const RawConfig config(kAircraftDocument);
    REQUIRE(config.view.has_value());
    Fixture fixture(config, kWindOnce);

    REQUIRE(fixture.Physics().SetBodyLinearVelocity(fixture.Body(), glm::vec3{ 0.0f, 0.0f, -50.0f }));
    fixture.Fixed();
    fixture.Step();
    fixture.Script();

    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "ok"));
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "airspeed") == Catch::Approx(60.0f).margin(0.5f));

    fixture.Step();
    fixture.Script();

    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "airspeed") == Catch::Approx(50.0f).margin(1.0f));
}

TEST_CASE("reset_aircraft moves the body and sets its speed and throttle", "[script][flight][reset]")
{
    const RawConfig config(kAircraftDocument);
    REQUIRE(config.view.has_value());
    Fixture fixture(config, kReset);

    fixture.Start();

    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "ok"));
    CHECK(fixture.world.GetComponent<mir::Transform>(fixture.entity)->Position() == mir::WorldVec3(20.0, 5.0, 0.0));

    const std::optional<mir::WorldPosition> at = fixture.Physics().BodyPosition(fixture.Body());
    REQUIRE(at.has_value());
    CHECK(at->Meters().x == Catch::Approx(20.0).margin(1e-3));

    const glm::vec3 velocity = fixture.Physics().BodyLinearVelocity(fixture.Body());
    CHECK(glm::length(velocity) == Catch::Approx(30.0f).margin(0.5f));
    CHECK(velocity.z == Catch::Approx(-30.0f).margin(0.5f));

    CHECK(fixture.Aircraft().runtime.actuators.throttle == Catch::Approx(0.5f));
    CHECK(fixture.Aircraft().runtime.propulsion.engines[0].rpm == Catch::Approx(1700.0f).margin(1.0f));
}

TEST_CASE("aircraft touchdowns appear after a landing and last one frame", "[script][flight][touchdown]")
{
    const RawConfig config(kAircraftDocument);
    REQUIRE(config.view.has_value());
    Fixture fixture(config, kReadTouchdowns, true, 0.62);
    fixture.AddGround();
    REQUIRE(fixture.Physics().SetBodyLinearVelocity(fixture.Body(), glm::vec3{ 0.0f, -1.5f, 0.0f }));

    for (int frame = 0; frame < 20 && ReadProbeField<int32_t>(fixture.world, fixture.entity, "count") == 0; ++frame)
    {
        fixture.Step();
        fixture.Script();
    }

    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "ok"));
    CHECK(ReadProbeField<int32_t>(fixture.world, fixture.entity, "count") == 1);
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "sink") == Catch::Approx(1.5f).margin(0.5f));

    fixture.Step();
    fixture.Script();
    CHECK(ReadProbeField<int32_t>(fixture.world, fixture.entity, "count") == 0);
}

TEST_CASE("aircraft controls read the held controls and the actuator state", "[script][flight][controls]")
{
    const RawConfig config(kAircraftDocument);
    REQUIRE(config.view.has_value());
    Fixture fixture(config, kReadControls);

    mir::AircraftRuntime &runtime = fixture.Aircraft().runtime;
    runtime.controls.pitch = 0.5f;
    runtime.controls.roll = -0.25f;
    runtime.controls.yaw = 0.75f;
    runtime.controls.throttleUp = true;
    runtime.controls.throttleDown = false;
    runtime.controls.brake = true;
    runtime.actuators.throttle = 0.4f;
    runtime.actuators.flapDetent = 2;
    runtime.actuators.trim = glm::radians(5.0f);

    fixture.Script();

    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "ok"));
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "pitch") == Catch::Approx(0.5f));
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "throttle") == Catch::Approx(0.4f));
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "trim") == Catch::Approx(5.0f).margin(1e-4f));
    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "brake"));
    CHECK(ReadProbeField<int32_t>(fixture.world, fixture.entity, "detent") == 2);
}

TEST_CASE("the flight bindings are inert on an entity with no Aircraft", "[script][flight][validation]")
{
    const RawConfig config(kAircraftDocument);
    REQUIRE(config.view.has_value());
    Fixture fixture(config, kNoAircraft, false);

    fixture.Start();

    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "ok"));
}

TEST_CASE("a bad argument is refused without changing the aircraft", "[script][flight][validation]")
{
    const RawConfig config(kAircraftDocument);
    REQUIRE(config.view.has_value());
    Fixture fixture(config, kBadArguments);

    fixture.Start();

    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "ok"));
    CHECK(fixture.Aircraft().runtime.additionalWind == glm::vec3{ 0.0f });
    CHECK(fixture.world.GetComponent<mir::Transform>(fixture.entity)->Position().x == Catch::Approx(0.0).margin(1e-6));
}
