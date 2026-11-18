/**
 * @file AirData.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight/AirData.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/AirData.h>

#include <flight/AircraftRuntime.h>
#include <flight/FlightEnvironment.h>
#include <flight/FlightModule.h>
#include <flight/components/Aircraft.h>

#include <assets/AssetBlob.h>
#include <core/ecs/CommandBuffer.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <physics/ComponentRegistration.h>
#include <physics/PhysicsStepGate.h>
#include <physics/PhysicsSystem.h>
#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/constants.hpp>
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

    std::string_view kFlightConfig = R"({
        "version": 1,
        "mass": 1200.0,
        "inertia": { "diagonal": [1000.0, 2000.0, 2500.0] },
        "centerOfMass": [0.5, 0.0, 0.2],
        "reference": { "area": 16.0, "span": 11.0, "chord": 1.5 },
        "camera": { "eye": [0.8, 0.0, -0.4], "chaseDistance": 12.0, "chaseHeight": 3.0 },
        "throttleRate": 0.5,
        "actuators": {
            "elevator": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 60.0, "delay": 0.05 },
            "aileron": { "maxDeflectionDeg": 20.0, "rateDegPerSec": 80.0, "delay": 0.05 },
            "rudder": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 60.0, "delay": 0.05 },
            "flaps": { "detentsDeg": [0.0, 10.0, 20.0, 30.0], "rateDegPerSec": 5.0 },
            "trim": { "stepDeg": 0.5, "maxDeg": 10.0 }
        }
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

    mir::AircraftState LevelFlight()
    {
        mir::AircraftState state;
        state.rotation = mir::kIdentityRotation;
        state.linearVelocity = glm::vec3{ 0.0f, 0.0f, -50.0f };
        return state;
    }

    void CheckFinite(const mir::AirData &airData)
    {
        CHECK(std::isfinite(airData.relativeVelocityBody.x));
        CHECK(std::isfinite(airData.relativeVelocityBody.y));
        CHECK(std::isfinite(airData.relativeVelocityBody.z));
        CHECK(std::isfinite(airData.trueAirspeed));
        CHECK(std::isfinite(airData.angleOfAttack));
        CHECK(std::isfinite(airData.sideslip));
        CHECK(std::isfinite(airData.dynamicPressure));
        CHECK(std::isfinite(airData.density));
        CHECK(std::isfinite(airData.speedOfSound));
        CHECK(std::isfinite(airData.mach));
        CHECK(std::isfinite(airData.altitude));
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
                { .onAdd = &mir::PhysicsSystem::OnRigidBodyAdded, .onRemove = &mir::PhysicsSystem::OnRigidBodyRemoved });
            world.EmplaceResource<mir::EngineModeState>();
            mir::SetEngineMode(world, mir::EngineMode::Play);
            mir::SetPlayState(world, mir::PlayState::Running);
            world.Resource<mir::PhysicsWorld>().SetSimulationLive(true);

            scheduler.AddSystem<mir::PhysicsSystem>(mir::SystemPhase::FixedUpdate, mir::SystemOrder{mir::SystemOrder::kNormal});
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

        mir::Entity AddAircraft(std::string_view config = {})
        {
            const mir::Entity entity = world.CreateEntity();
            mir::AddTransform(world, entity, mir::Transform{ mir::WorldVec3{0.0} });
            world.AddComponent<mir::RigidBody>(entity, mir::RigidBody{ .motion = mir::PhysicsMotion::Dynamic,
                                                                      .shape = mir::PhysicsShape::Box,
                                                                      .allowSleeping = false });
            mir::Aircraft aircraft;
            SetConfig(aircraft, config);
            world.AddComponent<mir::Aircraft>(entity, aircraft);
            return entity;
        }
    };
}

TEST_CASE("A headwind adds to the true airspeed", "[flight][airdata]")
{
    const mir::AtmosphereSample atmosphere = mir::SampleAtmosphere(mir::FlightEnvironment{}, 0.0);
    const mir::AirData airData = mir::ComputeAirData(LevelFlight(), glm::vec3{ 0.0f, 0.0f, 10.0f }, atmosphere);

    CHECK(airData.trueAirspeed == Catch::Approx(60.0f).margin(kTolerance));
    CHECK(airData.relativeVelocityBody.x == Catch::Approx(60.0f).margin(kTolerance));
    CHECK(airData.relativeVelocityBody.y == Catch::Approx(0.0f).margin(kTolerance));
    CHECK(airData.relativeVelocityBody.z == Catch::Approx(0.0f).margin(kTolerance));
}

TEST_CASE("An updraft raises the angle of attack", "[flight][airdata]")
{
    const mir::AtmosphereSample atmosphere = mir::SampleAtmosphere(mir::FlightEnvironment{}, 0.0);
    const mir::AirData airData = mir::ComputeAirData(LevelFlight(), glm::vec3{ 0.0f, 5.0f, 0.0f }, atmosphere);

    CHECK(airData.angleOfAttack == Catch::Approx(std::atan(5.0f / 50.0f)).margin(kTolerance));
    CHECK(airData.sideslip == Catch::Approx(0.0f).margin(kTolerance));
}

TEST_CASE("A crosswind from the right gives negative sideslip", "[flight][airdata]")
{
    const mir::AtmosphereSample atmosphere = mir::SampleAtmosphere(mir::FlightEnvironment{}, 0.0);
    const mir::AirData airData = mir::ComputeAirData(LevelFlight(), glm::vec3{ 5.0f, 0.0f, 0.0f }, atmosphere);

    CHECK(airData.sideslip == Catch::Approx(-std::atan(5.0f / 50.0f)).margin(kTolerance));
}

TEST_CASE("A yawed aircraft gets the body-frame air data", "[flight][airdata]")
{
    const glm::quat yaw = glm::angleAxis(glm::radians(90.0f), glm::vec3{ 0.0f, 1.0f, 0.0f });
    mir::AircraftState state;
    state.rotation = yaw;
    state.linearVelocity = yaw * glm::vec3{ 0.0f, 0.0f, -50.0f };

    const mir::AtmosphereSample atmosphere = mir::SampleAtmosphere(mir::FlightEnvironment{}, 0.0);
    const mir::AirData airData = mir::ComputeAirData(state, glm::vec3{ 0.0f, 5.0f, 0.0f }, atmosphere);

    CHECK(airData.relativeVelocityBody.x == Catch::Approx(50.0f).margin(kTolerance));
    CHECK(airData.angleOfAttack == Catch::Approx(std::atan(5.0f / 50.0f)).margin(kTolerance));
    CHECK(airData.sideslip == Catch::Approx(0.0f).margin(kTolerance));
}

TEST_CASE("Air data stays finite at rest and in reverse flow", "[flight][airdata]")
{
    const mir::AtmosphereSample atmosphere = mir::SampleAtmosphere(mir::FlightEnvironment{}, 0.0);

    SECTION("At rest")
    {
        mir::AircraftState state;
        state.rotation = mir::kIdentityRotation;
        const mir::AirData airData = mir::ComputeAirData(state, glm::vec3{ 0.0f }, atmosphere);

        CheckFinite(airData);
        CHECK(airData.trueAirspeed == Catch::Approx(0.0f).margin(kTolerance));
        CHECK(airData.angleOfAttack == 0.0f);
        CHECK(airData.sideslip == 0.0f);
    }

    SECTION("Pure reverse flow")
    {
        mir::AircraftState state;
        state.rotation = mir::kIdentityRotation;
        state.linearVelocity = glm::vec3{ 0.0f, 0.0f, 50.0f };
        const mir::AirData airData = mir::ComputeAirData(state, glm::vec3{ 0.0f }, atmosphere);

        CheckFinite(airData);
        CHECK(airData.trueAirspeed == Catch::Approx(50.0f).margin(kTolerance));
    }
}

TEST_CASE("Isa gives sea-level density and speed of sound", "[flight][airdata]")
{
    const mir::AtmosphereSample sample = mir::SampleAtmosphere(mir::FlightEnvironment{}, 0.0);

    CHECK(sample.altitude == 0.0);
    CHECK(sample.density == Catch::Approx(1.225f).margin(1e-3f));
    CHECK(sample.speedOfSound == Catch::Approx(340.3f).margin(1e-2f));
}

TEST_CASE("Isa thins with altitude", "[flight][airdata]")
{
    const mir::AtmosphereSample troposphere = mir::SampleAtmosphere(mir::FlightEnvironment{}, 5000.0);
    CHECK(troposphere.altitude == 5000.0);
    CHECK(troposphere.density == Catch::Approx(0.7364f).epsilon(0.005));

    const mir::AtmosphereSample stratosphere = mir::SampleAtmosphere(mir::FlightEnvironment{}, 15000.0);
    CHECK(stratosphere.altitude == 15000.0);
    CHECK(stratosphere.density == Catch::Approx(0.1948f).epsilon(0.01));
}

TEST_CASE("The sea level offset moves the altitude", "[flight][airdata]")
{
    mir::FlightEnvironment environment;
    environment.seaLevelY = 100.0;

    const mir::AtmosphereSample sample = mir::SampleAtmosphere(environment, 5100.0);

    CHECK(sample.altitude == 5000.0);
    CHECK(sample.density == Catch::Approx(0.7364f).epsilon(0.005));
}

TEST_CASE("The constant model ignores altitude", "[flight][airdata]")
{
    mir::FlightEnvironment environment;
    environment.densityModel = mir::DensityModel::Constant;
    environment.constantDensity = 0.4f;

    const mir::AtmosphereSample low = mir::SampleAtmosphere(environment, 0.0);
    const mir::AtmosphereSample high = mir::SampleAtmosphere(environment, 12000.0);

    CHECK(low.density == Catch::Approx(0.4f).margin(kTolerance));
    CHECK(high.density == Catch::Approx(0.4f).margin(kTolerance));
    CHECK(high.speedOfSound == Catch::Approx(340.29f).margin(kTolerance));
}

TEST_CASE("The gate consumes the additional wind after each step", "[flight][airdata]")
{
    const RawConfig config(kFlightConfig);
    REQUIRE(config.view.has_value());

    FlightFixture fixture;
    mir::FlightModule flight(fixture.ModuleContext(),
        [&config](std::string_view path, std::string &) -> const mir::AssetBlobView *
        {
            return path == "plane.aircraft.json" ? &*config.view : nullptr;
        });
    fixture.Start();
    const mir::Entity entity = fixture.AddAircraft("plane.aircraft.json");

    mir::FlightEnvironment &environment = fixture.world.Resource<mir::FlightEnvironment>();
    REQUIRE(environment.wind.z == 0.0f);
    environment.wind = glm::vec3{ 0.0f, 0.0f, -4.0f };

    mir::Aircraft *aircraft = fixture.world.GetComponent<mir::Aircraft>(entity);
    REQUIRE(aircraft != nullptr);
    aircraft->runtime.additionalWind = glm::vec3{ 0.0f, 0.0f, -6.0f };

    fixture.Run(1);

    const mir::AircraftRuntime &firstStep = fixture.world.GetComponent<mir::Aircraft>(entity)->runtime;
    CHECK(firstStep.airData.trueAirspeed == Catch::Approx(10.0f).margin(kTolerance));
    CHECK(firstStep.additionalWind.x == 0.0f);
    CHECK(firstStep.additionalWind.y == 0.0f);
    CHECK(firstStep.additionalWind.z == 0.0f);
    CHECK(environment.wind.x == 0.0f);
    CHECK(environment.wind.y == 0.0f);
    CHECK(environment.wind.z == Catch::Approx(-4.0f).margin(kTolerance));

    fixture.Run(1);

    const mir::AircraftRuntime &secondStep = fixture.world.GetComponent<mir::Aircraft>(entity)->runtime;
    CHECK(secondStep.airData.trueAirspeed == Catch::Approx(4.0f).margin(kTolerance));
    CHECK(environment.wind.z == Catch::Approx(-4.0f).margin(kTolerance));
}
