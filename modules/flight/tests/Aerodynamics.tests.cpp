/**
 * @file Aerodynamics.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight/AerodynamicModel.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/AerodynamicModel.h>

#include <flight/AircraftRuntime.h>
#include <flight/FlightModule.h>
#include <flight/components/Aircraft.h>

#include <assets/AssetBlob.h>
#include <core/ecs/CommandBuffer.h>
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
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr float kStep = 1.0f / 60.0f;
    constexpr float kTolerance = 1e-4f;

    mir::AirData AirAt(float speed, float alphaDegrees)
    {
        mir::AirData air;
        const float alpha = glm::radians(alphaDegrees);
        air.relativeVelocityBody = glm::vec3{ speed * std::cos(alpha), 0.0f, speed * std::sin(alpha) };
        air.trueAirspeed = speed;
        air.dynamicPressure = 0.5f * 1.225f * speed * speed;
        air.density = 1.225f;
        return air;
    }

    mir::AeroInput FastInput()
    {
        mir::AeroInput input;
        input.air = AirAt(50.0f, 0.0f);
        input.elevatorMax = glm::radians(25.0f);
        input.aileronMax = glm::radians(20.0f);
        input.rudderMax = glm::radians(25.0f);
        input.area = 16.0f;
        input.span = 11.0f;
        input.chord = 1.5f;
        return input;
    }

    mir::AerodynamicProfile MakeL1Profile()
    {
        mir::AerodynamicProfile profile;
        profile.l1 = mir::L1Profile{ 0.4f, 0.04f, 3.0f, 4.0f, 2.0f, 2000.0f, 4000.0f, 2000.0f };
        return profile;
    }

    void CheckVector(const glm::vec3 &actual, const glm::vec3 &expected, float margin = kTolerance)
    {
        CHECK(actual.x == Catch::Approx(expected.x).margin(margin));
        CHECK(actual.y == Catch::Approx(expected.y).margin(margin));
        CHECK(actual.z == Catch::Approx(expected.z).margin(margin));
    }

    std::string_view kL1Config = R"({
        "version": 1,
        "mass": 1200.0,
        "inertia": { "diagonal": [1000.0, 2000.0, 2500.0] },
        "centerOfMass": [0.0, 0.0, 0.0],
        "reference": { "area": 16.0, "span": 11.0, "chord": 1.5 },
        "camera": { "eye": [0.8, 0.0, -0.4], "chaseDistance": 12.0, "chaseHeight": 3.0 },
        "throttleRate": 0.5,
        "actuators": {
            "elevator": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 60.0, "delay": 0.05 },
            "aileron": { "maxDeflectionDeg": 20.0, "rateDegPerSec": 80.0, "delay": 0.05 },
            "rudder": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 60.0, "delay": 0.05 },
            "flaps": { "detentsDeg": [0.0, 10.0, 20.0, 30.0], "rateDegPerSec": 5.0 },
            "trim": { "stepDeg": 0.5, "maxDeg": 10.0 }
        },
        "engines": [],
        "gear": [],
        "aero": { "L1": { "CL": 0.4, "CD": 0.04, "Ka": 3.0, "Ke": 4.0, "Kr": 2.0,
                           "Kp": 2000.0, "Kq": 4000.0, "Kn": 2000.0 } }
    })";

    struct RawConfig
    {
        explicit RawConfig(std::string_view json)
            : raw(mir::BuildAssetBlob(mir::kRawAssetTypeTag, mir::kRawAssetContentVersion,
                  std::span<const std::byte>{ reinterpret_cast<const std::byte *>(json.data()), json.size() })),
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

    struct FlightFixture
    {
        mir::World world;
        mir::CommandBuffer commands;
        mir::SystemScheduler scheduler;
        mir::SystemContext context{ world, commands, kStep, 0.0, 0 };
        mir::Registration hooks;

        FlightFixture()
        {
            mir::RegisterPhysicsComponents();
            world.EmplaceResource<mir::PhysicsWorld>(mir::PhysicsWorldDesc{ .mGravity = glm::vec3{0.0f} });
            hooks = world.SetComponentHooks<mir::RigidBody>(
                { .onAdd = &mir::PhysicsSystem::OnRigidBodyAdded,
                    .onRemove = &mir::PhysicsSystem::OnRigidBodyRemoved });
            world.EmplaceResource<mir::EngineModeState>();
            mir::SetEngineMode(world, mir::EngineMode::Play);
            mir::SetPlayState(world, mir::PlayState::Running);
            world.Resource<mir::PhysicsWorld>().SetSimulationLive(true);

            scheduler.AddSystem<mir::PhysicsSystem>(mir::SystemPhase::FixedUpdate,
                mir::SystemOrder{ mir::SystemOrder::kNormal });
        }

        ~FlightFixture()
        {
            if (scheduler.Started())
                scheduler.Stop(context);
        }

        void Start() { scheduler.Start(context); }

        mir::ModuleContext ModuleContext() { return { world, scheduler }; }

        void Run(int steps)
        {
            for (int i = 0; i < steps; ++i)
                scheduler.Update(context, 1, kStep);
        }

        mir::PhysicsWorld &Physics() { return world.Resource<mir::PhysicsWorld>(); }

        mir::Entity AddAircraft(std::string_view config)
        {
            const mir::Entity entity = world.CreateEntity();
            mir::AddTransform(world, entity, mir::Transform{ mir::WorldVec3{ 0.0 } });
            world.AddComponent<mir::RigidBody>(entity, mir::RigidBody{ .motion = mir::PhysicsMotion::Dynamic,
                                                                      .shape = mir::PhysicsShape::Box,
                                                                      .allowSleeping = false });
            mir::Aircraft aircraft;
            SetConfig(aircraft, config);
            aircraft.tier = mir::AerodynamicTier::L1;
            aircraft.playerInput = false;
            world.AddComponent<mir::Aircraft>(entity, aircraft);
            return entity;
        }
    };
}

TEST_CASE("The wind axes turn the flow into body-frame forces", "[flight][aero]")
{
    SECTION("level flow gives up lift, backward drag and right side force")
    {
        const mir::WindAxisForces axes = mir::WindAxesToBody(AirAt(50.0f, 0.0f), 2.0f, 1.0f, 0.5f);
        CheckVector(axes.lift, glm::vec3{ 0.0f, 0.0f, -2.0f });
        CheckVector(axes.drag, glm::vec3{ -1.0f, 0.0f, 0.0f });
        CheckVector(axes.side, glm::vec3{ 0.0f, 0.5f, 0.0f });
    }

    SECTION("lift stays perpendicular to a climbing flow")
    {
        const float alpha = glm::radians(10.0f);
        const mir::WindAxisForces axes = mir::WindAxesToBody(AirAt(50.0f, 10.0f), 2.0f, 1.0f, 0.0f);
        CheckVector(axes.lift, glm::vec3{ std::sin(alpha), 0.0f, -std::cos(alpha) } * 2.0f);
        CheckVector(axes.drag, glm::vec3{ -std::cos(alpha), 0.0f, -std::sin(alpha) });
    }

    SECTION("below the minimum airspeed the wind axes are undefined")
    {
        const mir::WindAxisForces axes = mir::WindAxesToBody(AirAt(0.0f, 0.0f), 2.0f, 1.0f, 0.5f);
        CheckVector(axes.lift, glm::vec3{ 0.0f });
        CheckVector(axes.drag, glm::vec3{ 0.0f });
        CheckVector(axes.side, glm::vec3{ 0.0f });
    }
}

TEST_CASE("L1 is a pure function of its input", "[flight][aero]")
{
    const mir::IAerodynamicModel &model = mir::AerodynamicModelFor(mir::AerodynamicTier::L1);
    const mir::AerodynamicProfile profile = MakeL1Profile();
    mir::AeroInput input = FastInput();
    input.angularVelocityBody = glm::vec3{ 0.1f, -0.2f, 0.3f };
    input.elevator = 0.1f;
    input.aileron = -0.15f;
    input.rudder = 0.2f;

    const mir::ForceMoment first = model.Evaluate(input, profile);
    const mir::ForceMoment second = model.Evaluate(input, profile);

    CHECK(first.force == second.force);
    CHECK(first.moment == second.moment);
    CHECK(first.sources.lift == second.sources.lift);
    CHECK(first.sources.drag == second.sources.drag);
    CHECK(first.sources.side == second.sources.side);
    CHECK(first.sources.controlMoment == second.sources.controlMoment);
    CHECK(first.sources.dampingMoment == second.sources.dampingMoment);
    CHECK(first.sources.stabilityMoment == second.sources.stabilityMoment);
}

TEST_CASE("L1 force and control moments follow the dynamic pressure", "[flight][aero]")
{
    const mir::IAerodynamicModel &model = mir::AerodynamicModelFor(mir::AerodynamicTier::L1);
    const mir::AerodynamicProfile profile = MakeL1Profile();

    mir::AeroInput slow = FastInput();
    slow.elevator = 0.1f;
    slow.aileron = 0.1f;
    slow.rudder = 0.1f;
    mir::AeroInput fast = slow;
    fast.air = AirAt(100.0f, 0.0f);

    const mir::ForceMoment slowResult = model.Evaluate(slow, profile);
    const mir::ForceMoment fastResult = model.Evaluate(fast, profile);

    CHECK(glm::length(fastResult.sources.lift) == Catch::Approx(4.0f * glm::length(slowResult.sources.lift)));
    CHECK(glm::length(fastResult.sources.drag) == Catch::Approx(4.0f * glm::length(slowResult.sources.drag)));
    CHECK(glm::length(fastResult.sources.controlMoment) ==
        Catch::Approx(4.0f * glm::length(slowResult.sources.controlMoment)));
}

TEST_CASE("L1 control deflections give moments in their positive direction", "[flight][aero]")
{
    const mir::IAerodynamicModel &model = mir::AerodynamicModelFor(mir::AerodynamicTier::L1);
    const mir::AerodynamicProfile profile = MakeL1Profile();
    mir::AeroInput input = FastInput();
    input.elevator = 0.1f;
    input.aileron = 0.1f;
    input.rudder = 0.1f;

    const mir::ForceMoment result = model.Evaluate(input, profile);

    CHECK(result.sources.controlMoment.x > 0.0f);
    CHECK(result.sources.controlMoment.y > 0.0f);
    CHECK(result.sources.controlMoment.z > 0.0f);
    CHECK(result.moment.x > 0.0f);
    CHECK(result.moment.y > 0.0f);
    CHECK(result.moment.z > 0.0f);
}

TEST_CASE("L1 damping opposes the body rates at every airspeed", "[flight][aero]")
{
    const mir::IAerodynamicModel &model = mir::AerodynamicModelFor(mir::AerodynamicTier::L1);
    const mir::AerodynamicProfile profile = MakeL1Profile();
    mir::AeroInput slow = FastInput();
    slow.air = AirAt(10.0f, 0.0f);
    slow.angularVelocityBody = glm::vec3{ 1.0f, 1.0f, 1.0f };
    mir::AeroInput fast = slow;
    fast.air = AirAt(50.0f, 0.0f);

    const mir::ForceMoment lowSpeed = model.Evaluate(slow, profile);
    const mir::ForceMoment highSpeed = model.Evaluate(fast, profile);

    CHECK(lowSpeed.sources.dampingMoment.x < 0.0f);
    CHECK(lowSpeed.sources.dampingMoment.y < 0.0f);
    CHECK(lowSpeed.sources.dampingMoment.z < 0.0f);
    CHECK(highSpeed.sources.dampingMoment == lowSpeed.sources.dampingMoment);
    CHECK(highSpeed.moment == lowSpeed.moment);
}

TEST_CASE("L1 keeps damping when the airspeed is zero", "[flight][aero]")
{
    const mir::IAerodynamicModel &model = mir::AerodynamicModelFor(mir::AerodynamicTier::L1);
    const mir::AerodynamicProfile profile = MakeL1Profile();
    mir::AeroInput input = FastInput();
    input.air = AirAt(0.0f, 0.0f);
    input.angularVelocityBody = glm::vec3{ 1.0f, -1.0f, 1.0f };
    input.elevator = 0.1f;
    input.aileron = 0.1f;
    input.rudder = 0.1f;

    const mir::ForceMoment result = model.Evaluate(input, profile);

    CHECK(result.force == glm::vec3{ 0.0f });
    CHECK(result.sources.controlMoment == glm::vec3{ 0.0f });
    CHECK(result.sources.dampingMoment.x < 0.0f);
    CHECK(result.sources.dampingMoment.y > 0.0f);
    CHECK(result.sources.dampingMoment.z < 0.0f);
    CHECK(result.moment == result.sources.dampingMoment);
}

TEST_CASE("L1 sources add up to the totals", "[flight][aero]")
{
    const mir::IAerodynamicModel &model = mir::AerodynamicModelFor(mir::AerodynamicTier::L1);
    const mir::AerodynamicProfile profile = MakeL1Profile();
    mir::AeroInput input = FastInput();
    input.angularVelocityBody = glm::vec3{ 0.2f, -0.3f, 0.1f };
    input.elevator = 0.1f;
    input.aileron = 0.05f;
    input.rudder = -0.1f;

    const mir::ForceMoment result = model.Evaluate(input, profile);

    CheckVector(result.force, result.sources.lift + result.sources.drag + result.sources.side);
    CheckVector(result.moment, result.sources.controlMoment + result.sources.dampingMoment
            + result.sources.stabilityMoment);
    CHECK(result.sources.stabilityMoment == glm::vec3{ 0.0f });
}

TEST_CASE("A tier without a profile yields nothing", "[flight][aero]")
{
    const mir::AerodynamicProfile empty;
    const mir::AeroInput input = FastInput();

    const mir::ForceMoment l1 = mir::AerodynamicModelFor(mir::AerodynamicTier::L1).Evaluate(input, empty);
    CHECK(l1.force == glm::vec3{ 0.0f });
    CHECK(l1.moment == glm::vec3{ 0.0f });

    const mir::AerodynamicProfile profile = MakeL1Profile();
    const mir::ForceMoment l2 = mir::AerodynamicModelFor(mir::AerodynamicTier::L2).Evaluate(input, profile);
    CHECK(l2.force == glm::vec3{ 0.0f });
    CHECK(l2.moment == glm::vec3{ 0.0f });
}

TEST_CASE("An L1 aircraft pitches up on a positive elevator", "[flight][aero]")
{
    const RawConfig config(kL1Config);
    REQUIRE(config.view.has_value());

    FlightFixture fixture;
    mir::FlightModule flight(fixture.ModuleContext(),
        [&config](std::string_view path, std::string &) -> const mir::AssetBlobView *
        {
            return path == "l1.aircraft.json" ? &*config.view : nullptr;
        });
    fixture.Start();
    const mir::Entity entity = fixture.AddAircraft("l1.aircraft.json");
    fixture.world.GetComponent<mir::Aircraft>(entity)->runtime.controls.pitch = 1.0f;

    const mir::BodyHandle body = fixture.world.GetComponent<mir::RigidBody>(entity)->runtime.body;
    fixture.Physics().SetBodyLinearVelocity(body, glm::vec3{ 0.0f, 0.0f, -50.0f });
    fixture.Run(30);

    const glm::vec3 nose = fixture.Physics().BodyRotation(body) * glm::vec3{ 0.0f, 0.0f, -1.0f };
    CHECK(nose.y > 0.0f);
    CHECK(fixture.world.GetComponent<mir::Aircraft>(entity)->runtime.state.angularVelocityBody.y > 0.0f);
}

TEST_CASE("An L1 aircraft loses roll rate without controls", "[flight][aero]")
{
    const RawConfig config(kL1Config);
    REQUIRE(config.view.has_value());

    FlightFixture fixture;
    mir::FlightModule flight(fixture.ModuleContext(),
        [&config](std::string_view path, std::string &) -> const mir::AssetBlobView *
        {
            return path == "l1.aircraft.json" ? &*config.view : nullptr;
        });
    fixture.Start();
    const mir::Entity entity = fixture.AddAircraft("l1.aircraft.json");

    const mir::BodyHandle body = fixture.world.GetComponent<mir::RigidBody>(entity)->runtime.body;
    fixture.Physics().SetBodyAngularVelocity(body, glm::vec3{ 0.0f, 0.0f, -1.0f });
    fixture.Run(30);

    const float rollRate = fixture.world.GetComponent<mir::Aircraft>(entity)->runtime.state.angularVelocityBody.x;
    CHECK(rollRate > 0.0f);
    CHECK(rollRate < 0.5f);
}

TEST_CASE("Switching to a missing tier clears the last aerodynamic result", "[flight][aero]")
{
    const RawConfig config(kL1Config);
    REQUIRE(config.view.has_value());

    FlightFixture fixture;
    mir::FlightModule flight(fixture.ModuleContext(),
        [&config](std::string_view path, std::string &) -> const mir::AssetBlobView *
        {
            return path == "l1.aircraft.json" ? &*config.view : nullptr;
        });
    fixture.Start();
    const mir::Entity entity = fixture.AddAircraft("l1.aircraft.json");
    const mir::BodyHandle body = fixture.world.GetComponent<mir::RigidBody>(entity)->runtime.body;
    fixture.Physics().SetBodyLinearVelocity(body, glm::vec3{ 0.0f, 0.0f, -50.0f });
    fixture.Run(2);
    REQUIRE(glm::length(fixture.world.GetComponent<mir::Aircraft>(entity)->runtime.telemetry.aeroForces.force) > 0.0f);

    fixture.world.GetComponent<mir::Aircraft>(entity)->tier = mir::AerodynamicTier::L3;
    fixture.Run(1);

    const mir::AircraftRuntime &runtime = fixture.world.GetComponent<mir::Aircraft>(entity)->runtime;
    CHECK(glm::length(runtime.telemetry.aeroForces.force) == 0.0f);
    CHECK(glm::length(runtime.telemetry.aeroForces.moment) == 0.0f);
    CHECK(glm::length(runtime.aero.sources.lift) == 0.0f);
}
