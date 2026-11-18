/**
 * @file FlightActuators.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight/FlightActuators and the frame-to-step command handoff.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/FlightActuators.h>

#include <flight/AircraftConfig.h>
#include <flight/AircraftRuntime.h>
#include <flight/FlightInput.h>
#include <flight/FlightModule.h>
#include <flight/components/Aircraft.h>

#include <assets/AssetBlob.h>
#include <core/ecs/CommandBuffer.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <input/InputMap.h>
#include <input/InputState.h>
#include <physics/ComponentRegistration.h>
#include <physics/PhysicsSystem.h>
#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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

    std::string_view kFlightConfig = R"({
        "version": 1,
        "mass": 1200.0,
        "inertia": { "diagonal": [1000.0, 2000.0, 2500.0] },
        "centerOfMass": [0.5, 0.0, 0.2],
        "reference": { "area": 16.0, "span": 11.0, "chord": 1.5 },
        "camera": { "eye": [0.8, 0.0, -0.4], "chaseDistance": 12.0, "chaseHeight": 3.0 },
        "throttleRate": 0.5,
        "actuators": {
            "elevator": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 60.0, "delay": 0.0 },
            "aileron": { "maxDeflectionDeg": 20.0, "rateDegPerSec": 80.0, "delay": 0.0 },
            "rudder": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 60.0, "delay": 0.0 },
            "flaps": { "detentsDeg": [0.0, 10.0, 20.0, 30.0], "rateDegPerSec": 5.0 },
            "trim": { "stepDeg": 0.5, "maxDeg": 10.0 }
        },
        "engines": []
    })";

    mir::AircraftConfig MakeConfig()
    {
        mir::AircraftConfig config;
        config.throttleRate = 0.5f;
        config.actuators.elevator = { glm::radians(25.0f), glm::radians(60.0f), 0.0f };
        config.actuators.aileron = { glm::radians(20.0f), glm::radians(80.0f), 0.0f };
        config.actuators.rudder = { glm::radians(25.0f), glm::radians(60.0f), 0.0f };
        config.actuators.flaps.detents = { 0.0f, glm::radians(10.0f), glm::radians(20.0f), glm::radians(30.0f) };
        config.actuators.flaps.rate = glm::radians(5.0f);
        config.actuators.trim = { glm::radians(0.5f), glm::radians(10.0f) };
        return config;
    }

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
        mir::InputState &input = world.EmplaceResource<mir::InputState>();

        FlightFixture()
        {
            mir::RegisterPhysicsComponents();
            world.EmplaceResource<mir::PhysicsWorld>(mir::PhysicsWorldDesc{ .mGravity = glm::vec3{ 0.0f } });
            hooks = world.SetComponentHooks<mir::RigidBody>(
                { .onAdd = &mir::PhysicsSystem::OnRigidBodyAdded, .onRemove = &mir::PhysicsSystem::OnRigidBodyRemoved });
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

        void Frame(int fixedSteps) { scheduler.Update(context, static_cast<uint32_t>(fixedSteps), kStep); }

        mir::Entity AddAircraft(std::string_view config = {})
        {
            const mir::Entity entity = world.CreateEntity();
            mir::AddTransform(world, entity, mir::Transform{ mir::WorldVec3{ 0.0 } });
            world.AddComponent<mir::RigidBody>(entity, mir::RigidBody{ .motion = mir::PhysicsMotion::Dynamic,
                                                                      .shape = mir::PhysicsShape::Box,
                                                                      .allowSleeping = false });
            mir::Aircraft aircraft;
            SetConfig(aircraft, config);
            world.AddComponent<mir::Aircraft>(entity, aircraft);
            return entity;
        }
    };

    mir::ActionValue Pressed(float value = 1.0f)
    {
        mir::ActionValue action;
        action.down = true;
        action.justPressed = true;
        action.x = value;
        return action;
    }
}

TEST_CASE("Full stick back is rate limited and then settles", "[flight][actuators]")
{
    const mir::AircraftConfig config = MakeConfig();
    mir::ActuatorState state;
    mir::FlightCommandQueue commands;
    mir::FlightControls controls;
    controls.pitch = 1.0f;

    for (int i = 0; i < 6; ++i)
        mir::StepActuators(config, controls, commands, state, kStep);

    CHECK(state.elevator == Catch::Approx(glm::radians(6.0f)).margin(kTolerance));

    for (int i = 0; i < 300; ++i)
        mir::StepActuators(config, controls, commands, state, kStep);

    CHECK(state.elevator == Catch::Approx(config.actuators.elevator.maxDeflection).margin(kTolerance));
}

TEST_CASE("A non-zero delay filters the command before the rate limit", "[flight][actuators]")
{
    mir::AircraftConfig config = MakeConfig();
    config.actuators.elevator.delay = 0.05f;

    mir::ActuatorState state;
    mir::FlightCommandQueue commands;
    mir::FlightControls controls;
    controls.pitch = 1.0f;

    mir::StepActuators(config, controls, commands, state, kStep);

    const float lagged =
        config.actuators.elevator.maxDeflection * (1.0f - std::exp(-kStep / config.actuators.elevator.delay));
    const float expected = std::min(lagged, config.actuators.elevator.rate * kStep);
    CHECK(state.elevatorLag == Catch::Approx(lagged).margin(kTolerance));
    CHECK(state.elevator == Catch::Approx(expected).margin(kTolerance));
}

TEST_CASE("Trim commands apply in order and clamp", "[flight][actuators]")
{
    const mir::AircraftConfig config = MakeConfig();
    mir::ActuatorState state;
    mir::FlightCommandQueue commands;
    mir::FlightControls controls;

    commands.Push(mir::FlightCommand::TrimUp);
    commands.Push(mir::FlightCommand::TrimUp);
    mir::StepActuators(config, controls, commands, state, kStep);

    CHECK(state.trim == Catch::Approx(2.0f * config.actuators.trim.step).margin(kTolerance));
    CHECK(commands.count == 0);

    for (int round = 0; round < 3; ++round)
    {
        for (std::size_t i = 0; i < mir::kFlightCommandCapacity; ++i)
            commands.Push(mir::FlightCommand::TrimUp);
        mir::StepActuators(config, controls, commands, state, kStep);
    }
    CHECK(state.trim == Catch::Approx(config.actuators.trim.max).margin(kTolerance));

    for (int i = 0; i < 60; ++i)
        mir::StepActuators(config, controls, commands, state, kStep);
    CHECK(state.elevator == Catch::Approx(config.actuators.trim.max).margin(kTolerance));
}

TEST_CASE("Flap commands move the detent in order and clamp", "[flight][actuators]")
{
    const mir::AircraftConfig config = MakeConfig();
    mir::FlightCommandQueue commands;
    mir::FlightControls controls;

    SECTION("FlapsDown then FlapsUp ends at the starting detent")
    {
        mir::ActuatorState state;
        commands.Push(mir::FlightCommand::FlapsDown);
        commands.Push(mir::FlightCommand::FlapsUp);
        mir::StepActuators(config, controls, commands, state, kStep);
        CHECK(state.flapDetent == 0);
    }

    SECTION("FlapsUp then FlapsDown ends one detent down")
    {
        mir::ActuatorState state;
        commands.Push(mir::FlightCommand::FlapsUp);
        commands.Push(mir::FlightCommand::FlapsDown);
        mir::StepActuators(config, controls, commands, state, kStep);
        CHECK(state.flapDetent == 1);
    }

    SECTION("The detent stays inside the detent list")
    {
        mir::ActuatorState state;
        for (int i = 0; i < 10; ++i)
            commands.Push(mir::FlightCommand::FlapsDown);
        mir::StepActuators(config, controls, commands, state, kStep);
        CHECK(state.flapDetent == static_cast<int>(config.actuators.flaps.detents.size()) - 1);
        CHECK(state.flaps > 0.0f);

        for (int i = 0; i < 20; ++i)
            commands.Push(mir::FlightCommand::FlapsUp);
        mir::StepActuators(config, controls, commands, state, kStep);
        CHECK(state.flapDetent == 0);
    }
}

TEST_CASE("The command queue drops commands past its capacity", "[flight][actuators]")
{
    mir::FlightCommandQueue commands;
    for (int i = 0; i < 20; ++i)
        CHECK(commands.Push(mir::FlightCommand::TrimUp) == (i < static_cast<int>(mir::kFlightCommandCapacity)));

    REQUIRE(commands.count == mir::kFlightCommandCapacity);
    for (std::size_t i = 0; i < commands.count; ++i)
        CHECK(commands.commands[i] == mir::FlightCommand::TrimUp);
}

TEST_CASE("Throttle ramps at throttleRate and clamps", "[flight][actuators]")
{
    const mir::AircraftConfig config = MakeConfig();
    mir::ActuatorState state;
    mir::FlightCommandQueue commands;
    mir::FlightControls controls;
    controls.throttleUp = true;

    mir::StepActuators(config, controls, commands, state, 0.5f);
    CHECK(state.throttle == Catch::Approx(config.throttleRate * 0.5f).margin(kTolerance));

    for (int i = 0; i < 10; ++i)
        mir::StepActuators(config, controls, commands, state, 0.5f);
    CHECK(state.throttle == Catch::Approx(1.0f).margin(kTolerance));

    controls.throttleUp = false;
    controls.throttleDown = true;
    mir::StepActuators(config, controls, commands, state, 0.5f);
    CHECK(state.throttle == Catch::Approx(1.0f - config.throttleRate * 0.5f).margin(kTolerance));
}

TEST_CASE("Stick right and right rudder produce positive deflections", "[flight][actuators]")
{
    const mir::AircraftConfig config = MakeConfig();
    mir::ActuatorState state;
    mir::FlightCommandQueue commands;
    mir::FlightControls controls;
    controls.roll = 1.0f;
    controls.yaw = 1.0f;

    mir::StepActuators(config, controls, commands, state, kStep);

    CHECK(state.aileron > 0.0f);
    CHECK(state.rudder > 0.0f);
}

TEST_CASE("A frame without a fixed step keeps its commands queued", "[flight][actuators]")
{
    const RawConfig config(kFlightConfig);
    REQUIRE(config.view.has_value());

    FlightFixture fixture;
    mir::FlightModule flight(fixture.ModuleContext(),
        [&config](std::string_view path, std::string &) -> const mir::AssetBlobView *
        { return path == "plane.aircraft.json" ? &*config.view : nullptr; });
    fixture.Start();
    const mir::Entity entity = fixture.AddAircraft("plane.aircraft.json");

    fixture.input.Actions()[std::string(mir::kFlightTrimUpAction)] = Pressed();
    fixture.Frame(0);

    const mir::AircraftRuntime &runtime = fixture.world.GetComponent<mir::Aircraft>(entity)->runtime;
    CHECK(runtime.controls.pitch == 0.0f);
    REQUIRE(runtime.commands.count == 1);
    CHECK(runtime.commands.commands[0] == mir::FlightCommand::TrimUp);
    CHECK(runtime.actuators.elevator == 0.0f);

    fixture.input.Actions()[std::string(mir::kFlightTrimUpAction)].justPressed = false;
    fixture.Frame(1);

    const mir::AircraftRuntime &stepped = fixture.world.GetComponent<mir::Aircraft>(entity)->runtime;
    CHECK(stepped.commands.count == 0);
    CHECK(stepped.actuators.trim == Catch::Approx(glm::radians(0.5f)).margin(kTolerance));
}

TEST_CASE("A pressed FlightPitch ends with a positive elevator deflection", "[flight][actuators]")
{
    const RawConfig config(kFlightConfig);
    REQUIRE(config.view.has_value());

    FlightFixture fixture;
    mir::FlightModule flight(fixture.ModuleContext(),
        [&config](std::string_view path, std::string &) -> const mir::AssetBlobView *
        { return path == "plane.aircraft.json" ? &*config.view : nullptr; });
    fixture.Start();
    const mir::Entity entity = fixture.AddAircraft("plane.aircraft.json");

    fixture.input.Actions()[std::string(mir::kFlightPitchAction)] = Pressed();
    fixture.Frame(1);

    const mir::AircraftRuntime &runtime = fixture.world.GetComponent<mir::Aircraft>(entity)->runtime;
    REQUIRE(runtime.config != nullptr);
    CHECK(runtime.controls.pitch == 1.0f);
    CHECK(runtime.actuators.elevator > 0.0f);
}

TEST_CASE("An aircraft without playerInput ignores the input actions", "[flight][actuators]")
{
    const RawConfig config(kFlightConfig);
    REQUIRE(config.view.has_value());

    FlightFixture fixture;
    mir::FlightModule flight(fixture.ModuleContext(),
        [&config](std::string_view path, std::string &) -> const mir::AssetBlobView *
        { return path == "plane.aircraft.json" ? &*config.view : nullptr; });
    fixture.Start();
    const mir::Entity entity = fixture.AddAircraft("plane.aircraft.json");
    fixture.world.GetComponent<mir::Aircraft>(entity)->playerInput = false;

    fixture.input.Actions()[std::string(mir::kFlightPitchAction)] = Pressed();
    fixture.Frame(1);

    const mir::AircraftRuntime &runtime = fixture.world.GetComponent<mir::Aircraft>(entity)->runtime;
    CHECK(runtime.controls.pitch == 0.0f);
    CHECK(runtime.actuators.elevator == 0.0f);
}
