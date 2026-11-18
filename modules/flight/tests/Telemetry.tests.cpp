/**
 * @file Telemetry.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight/FlightTelemetry and flight/FlightReset.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/AircraftConfig.h>
#include <flight/AircraftRuntime.h>
#include <flight/AirData.h>
#include <flight/FlightFrames.h>
#include <flight/FlightModule.h>
#include <flight/FlightReset.h>
#include <flight/FlightTelemetry.h>
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
#include <renderer/CameraCut.h>
#include <renderer/components/Camera.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
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
    constexpr std::string_view kConfigPath = "telemetry.aircraft.json";

    constexpr std::string_view kGearPoints = R"([
        { "name": "nose", "mount": [1.5, 0.0, 0.0], "restLength": 0.6, "stiffness": 40000.0, "damping": 10000.0,
          "rollingFriction": 0.03, "sideFriction": 0.9, "brake": 0.5, "maxSteerDeg": 30.0 },
        { "name": "left", "mount": [-0.75, -1.2, 0.0], "restLength": 0.6, "stiffness": 40000.0, "damping": 10000.0,
          "rollingFriction": 0.03, "sideFriction": 0.9, "brake": 0.5, "maxSteerDeg": 0.0 },
        { "name": "right", "mount": [-0.75, 1.2, 0.0], "restLength": 0.6, "stiffness": 40000.0, "damping": 10000.0,
          "rollingFriction": 0.03, "sideFriction": 0.9, "brake": 0.5, "maxSteerDeg": 0.0 }
    ])";

    constexpr std::string_view kEngine = R"([{
        "position": [0.0, 0.0, 0.0], "direction": [1.0, 0.0, 0.0], "idleRpm": 700.0, "maxRpm": 2700.0,
        "rpmTau": 0.8, "staticThrust": [[0, 0], [1, 3200]], "speedFactor": [[0, 1.0], [60, 0.8]]
    }])";

    constexpr std::string_view kL2 = R"({ "L2": {
        "CL0": 0.25, "CLalpha": 5.0, "CD0": 0.03, "k": 0.05,
        "Cm0": 0.05, "Cmalpha": -0.6, "Cmq": -12, "Cmde": 1.2,
        "CYbeta": -0.4, "CYdr": -0.1, "Clbeta": -0.1, "Clp": -0.5, "Clda": 0.08,
        "Cnbeta": 0.12, "Cnr": -0.15, "Cndr": 0.08, "Cnda": -0.02,
        "minAirspeed": 10,
        "stall": {
            "positiveDeg": 15, "negativeDeg": -15, "blendDeg": 6,
            "controlScale": 0.4, "stabilityScale": 0.3,
            "postStall": [[-15, -0.9, 0.2, -0.15], [0, 0.25, 0.05, 0.05],
                          [15, 1.35, 0.16, -0.1], [21, 1.02, 0.24, -0.13]]
        },
        "flaps": [[0, 0, 0, 0, 0], [10, 0.25, 0.005, -0.01, -0.5],
                  [20, 0.5, 0.012, -0.02, -1], [30, 0.7, 0.02, -0.03, -1]]
    } })";

    std::string AircraftDocument(std::string_view marginDeg = "5", std::string_view engines = "[]",
        std::string_view gear = kGearPoints, bool withAero = true)
    {
        std::string document = R"({
            "version": 1,
            "mass": 1200.0,
            "inertia": { "diagonal": [2000.0, 3500.0, 2500.0] },
            "centerOfMass": [0.0, 0.0, 0.15],
            "reference": { "area": 16.0, "span": 11.0, "chord": 1.5 },
            "camera": { "eye": [0.8, 0.0, -0.4], "chaseDistance": 12.0, "chaseHeight": 3.0 },
            "throttleRate": 0.5,
            "stallWarningMarginDeg": )";
        document += marginDeg;
        document += R"(,
            "actuators": {
                "elevator": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 600.0, "delay": 0.0 },
                "aileron": { "maxDeflectionDeg": 20.0, "rateDegPerSec": 600.0, "delay": 0.0 },
                "rudder": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 600.0, "delay": 0.0 },
                "flaps": { "detentsDeg": [0.0, 10.0, 20.0], "rateDegPerSec": 5.0 },
                "trim": { "stepDeg": 0.5, "maxDeg": 10.0 }
            },
            "engines": )";
        document += engines;
        document += R"(,
            "gear": )";
        document += gear;
        if (withAero)
        {
            document += R"(,
            "aero": )";
            document += kL2;
        }
        document += "}";
        return document;
    }

    mir::AircraftConfig Config(std::string_view marginDeg = "5", std::string_view engines = "[]",
        std::string_view gear = kGearPoints, bool withAero = true)
    {
        mir::AircraftConfig config;
        std::string error;
        if (!mir::ParseAircraftConfig(AircraftDocument(marginDeg, engines, gear, withAero), config, error))
            FAIL(error);
        return config;
    }

    mir::AirData AirAt(float speed, float alpha)
    {
        mir::AirData air;
        air.trueAirspeed = speed;
        air.angleOfAttack = alpha;
        air.density = 1.225f;
        air.dynamicPressure = 0.5f * air.density * speed * speed;
        air.speedOfSound = 340.29f;
        air.mach = speed / air.speedOfSound;
        return air;
    }

    mir::FlightTelemetry TelemetryOf(const mir::AircraftConfig &config, const mir::AircraftState &state,
        const mir::AirData &air, const mir::AircraftRuntime &runtime)
    {
        mir::FlightTelemetry telemetry;
        mir::FillFlightTelemetry(config, state, air, 100.0f, true, runtime, telemetry);
        return telemetry;
    }

    glm::vec3 FloatMeters(const mir::WorldVec3 &position)
    {
        return glm::vec3{static_cast<float>(position.x), static_cast<float>(position.y),
            static_cast<float>(position.z)};
    }

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

    struct ConfigSource
    {
        const mir::AssetBlobView *view = nullptr;

        const mir::AssetBlobView *operator()(std::string_view path, std::string &) const
        {
            return path == kConfigPath ? view : nullptr;
        }
    };

    void SetConfig(mir::Aircraft &aircraft, std::string_view path)
    {
        std::memset(aircraft.config, 0, sizeof(aircraft.config));
        if (!path.empty())
            std::memcpy(aircraft.config, path.data(), std::min(path.size(), sizeof(aircraft.config) - 1));
    }

    struct TelemetryFixture
    {
        mir::World world;
        mir::CommandBuffer commands;
        mir::SystemScheduler scheduler;
        mir::SystemContext context{world, commands, kStep, 0.0, 0};
        mir::Registration hooks;

        TelemetryFixture()
        {
            mir::RegisterPhysicsComponents();
            world.EmplaceResource<mir::PhysicsWorld>(mir::PhysicsWorldDesc{.mGravity = glm::vec3{0.0f}});
            hooks = world.SetComponentHooks<mir::RigidBody>(
                {.onAdd = &mir::PhysicsSystem::OnRigidBodyAdded, .onRemove = &mir::PhysicsSystem::OnRigidBodyRemoved});
            world.EmplaceResource<mir::EngineModeState>();
            mir::SetEngineMode(world, mir::EngineMode::Play);
            mir::SetPlayState(world, mir::PlayState::Running);
            world.Resource<mir::PhysicsWorld>().SetSimulationLive(true);

            scheduler.AddSystem<mir::PhysicsSystem>(mir::SystemPhase::FixedUpdate,
                mir::SystemOrder{mir::SystemOrder::kNormal});
        }

        ~TelemetryFixture()
        {
            if (scheduler.Started())
                scheduler.Stop(context);
        }

        void Start() { scheduler.Start(context); }

        mir::ModuleContext ModuleContext() { return {world, scheduler}; }

        void Run(int steps)
        {
            for (int i = 0; i < steps; ++i)
                scheduler.Update(context, 1, kStep);
        }

        mir::PhysicsWorld &Physics() { return world.Resource<mir::PhysicsWorld>(); }

        void AddGround()
        {
            mir::BodyDesc desc;
            desc.mMotion = mir::PhysicsMotion::Static;
            desc.mShape = mir::PhysicsShape::Box;
            desc.mPosition = *mir::WorldPosition::FromMeters(mir::WorldVec3(0.0, -0.5, 0.0));
            desc.mHalfExtent = glm::vec3{200.0f, 0.5f, 200.0f};
            Physics().CreateBody(desc);
        }

        mir::Entity AddAircraft(double height, const glm::quat &rotation = mir::kIdentityRotation)
        {
            const mir::Entity entity = world.CreateEntity();
            mir::AddTransform(world, entity, mir::Transform{mir::WorldVec3(0.0, height, 0.0), rotation});
            mir::RigidBody rigidBody;
            rigidBody.motion = mir::PhysicsMotion::Dynamic;
            rigidBody.shape = mir::PhysicsShape::Box;
            rigidBody.halfExtent = glm::vec3{1.5f, 0.25f, 2.0f};
            rigidBody.allowSleeping = false;
            world.AddComponent<mir::RigidBody>(entity, rigidBody);
            mir::Aircraft aircraft;
            SetConfig(aircraft, kConfigPath);
            world.AddComponent<mir::Aircraft>(entity, aircraft);
            return entity;
        }
    };
}

TEST_CASE("Telemetry reports the ground below the aircraft", "[flight][telemetry]")
{
    const RawConfig config(AircraftDocument());
    REQUIRE(config.view.has_value());

    TelemetryFixture fixture;
    mir::FlightModule flight(fixture.ModuleContext(), ConfigSource{&*config.view});
    fixture.Start();
    fixture.AddGround();
    const mir::Entity entity = fixture.AddAircraft(6.0);

    fixture.Run(1);

    const mir::Aircraft *aircraft = fixture.world.GetComponent<mir::Aircraft>(entity);
    REQUIRE(aircraft != nullptr);
    REQUIRE(aircraft->runtime.config != nullptr);
    CHECK(aircraft->runtime.telemetry.hasGround);
    CHECK(aircraft->runtime.telemetry.heightAboveGround == Catch::Approx(6.0f).margin(0.05f));
}

TEST_CASE("Telemetry reports no ground without one", "[flight][telemetry]")
{
    const RawConfig config(AircraftDocument());
    REQUIRE(config.view.has_value());

    TelemetryFixture fixture;
    mir::FlightModule flight(fixture.ModuleContext(), ConfigSource{&*config.view});
    fixture.Start();
    const mir::Entity entity = fixture.AddAircraft(6.0);

    fixture.Run(1);

    const mir::Aircraft *aircraft = fixture.world.GetComponent<mir::Aircraft>(entity);
    REQUIRE(aircraft != nullptr);
    CHECK_FALSE(aircraft->runtime.telemetry.hasGround);
    CHECK(aircraft->runtime.telemetry.heightAboveGround == 0.0f);
}

TEST_CASE("Telemetry derives the attitude from the body rotation", "[flight][telemetry]")
{
    const mir::AircraftConfig config = Config();
    const mir::AircraftRuntime runtime;
    const mir::AirData air = AirAt(50.0f, 0.0f);

    SECTION("the nose along world -Z is heading zero")
    {
        const mir::AircraftState state;
        const mir::FlightTelemetry telemetry = TelemetryOf(config, state, air, runtime);
        CHECK(telemetry.heading == Catch::Approx(0.0f).margin(1e-5f));
        CHECK(telemetry.pitch == Catch::Approx(0.0f).margin(1e-5f));
        CHECK(telemetry.roll == Catch::Approx(0.0f).margin(1e-5f));
    }

    SECTION("the nose toward world +X is a quarter turn")
    {
        mir::AircraftState state;
        state.rotation = glm::angleAxis(glm::radians(-90.0f), glm::vec3{0.0f, 1.0f, 0.0f});
        const mir::FlightTelemetry telemetry = TelemetryOf(config, state, air, runtime);
        CHECK(telemetry.heading == Catch::Approx(glm::half_pi<float>()).margin(1e-5f));
    }

    SECTION("a right wing down banks positive")
    {
        mir::AircraftState state;
        state.rotation = glm::angleAxis(glm::radians(-20.0f), glm::vec3{0.0f, 0.0f, 1.0f});
        const mir::FlightTelemetry telemetry = TelemetryOf(config, state, air, runtime);
        CHECK(telemetry.roll == Catch::Approx(glm::radians(20.0f)).margin(1e-4f));
    }
}

TEST_CASE("Level flight at one g reads a load factor of one", "[flight][telemetry]")
{
    const mir::AircraftConfig config = Config();
    mir::AircraftRuntime runtime;
    runtime.aero.force = glm::vec3{0.0f, 0.0f, -config.mass * 9.81f};
    const mir::AircraftState state;
    const mir::AirData air = AirAt(50.0f, 0.0f);

    const mir::FlightTelemetry telemetry = TelemetryOf(config, state, air, runtime);

    CHECK(telemetry.loadFactor == Catch::Approx(1.0f).margin(1e-4f));
}

TEST_CASE("The stall warning follows the configured margin", "[flight][telemetry]")
{
    const mir::AircraftState state;
    const mir::AircraftRuntime runtime;

    SECTION("no warning well below the critical angle")
    {
        const mir::AircraftConfig config = Config();
        const mir::FlightTelemetry telemetry = TelemetryOf(config, state, AirAt(50.0f, glm::radians(2.0f)), runtime);
        CHECK_FALSE(telemetry.stallWarning);
        CHECK(telemetry.stallMargin == Catch::Approx(glm::radians(13.0f)).margin(1e-4f));
    }

    SECTION("a warning inside the margin")
    {
        const mir::AircraftConfig config = Config();
        const mir::FlightTelemetry telemetry = TelemetryOf(config, state, AirAt(50.0f, glm::radians(12.0f)), runtime);
        CHECK(telemetry.stallWarning);
        CHECK(telemetry.stallMargin == Catch::Approx(glm::radians(3.0f)).margin(1e-4f));
    }

    SECTION("the threshold follows stallWarningMarginDeg")
    {
        const mir::AircraftConfig tight = Config("1");
        const mir::AircraftConfig wide = Config("10");
        const mir::AirData air = AirAt(50.0f, glm::radians(12.0f));
        CHECK_FALSE(TelemetryOf(tight, state, air, runtime).stallWarning);
        CHECK(TelemetryOf(wide, state, air, runtime).stallWarning);
    }

    SECTION("the warning needs a meaningful airspeed")
    {
        const mir::AircraftConfig config = Config();
        CHECK_FALSE(TelemetryOf(config, state, AirAt(0.0f, glm::radians(12.0f)), runtime).stallWarning);
    }

    SECTION("a config without an L2 profile never warns")
    {
        const mir::AircraftConfig config = Config("5", "[]", kGearPoints, false);
        const mir::FlightTelemetry telemetry = TelemetryOf(config, state, AirAt(50.0f, glm::radians(12.0f)), runtime);
        CHECK_FALSE(telemetry.stallWarning);
        CHECK(telemetry.stallMargin >= glm::radians(90.0f));
    }
}

TEST_CASE("ResetAircraft puts the body back and cuts the active camera", "[flight][telemetry][reset]")
{
    const RawConfig config(AircraftDocument("5", kEngine));
    REQUIRE(config.view.has_value());

    TelemetryFixture fixture;
    mir::FlightModule flight(fixture.ModuleContext(), ConfigSource{&*config.view});
    fixture.Start();
    const mir::Entity entity = fixture.AddAircraft(10.0);
    fixture.Run(1);

    REQUIRE(fixture.world.GetComponent<mir::Aircraft>(entity) != nullptr);
    REQUIRE(fixture.world.GetComponent<mir::Aircraft>(entity)->runtime.config != nullptr);

    const mir::Entity active = fixture.world.CreateEntity();
    fixture.world.AddComponent<mir::Camera>(active, mir::Camera{.mActive = true});
    const mir::Entity inactive = fixture.world.CreateEntity();
    fixture.world.AddComponent<mir::Camera>(inactive, mir::Camera{});

    mir::AircraftRuntime &runtime = fixture.world.GetComponent<mir::Aircraft>(entity)->runtime;
    runtime.actuators.elevator = 0.2f;
    runtime.actuators.aileron = -0.1f;
    runtime.actuators.rudder = 0.05f;
    runtime.actuators.flaps = glm::radians(20.0f);
    runtime.actuators.flapDetent = 2;
    runtime.actuators.trim = 0.2f;
    runtime.actuators.elevatorLag = 0.1f;
    runtime.commands.Push(mir::FlightCommand::FlapsDown);
    runtime.pendingTouchdowns.Push({});
    runtime.gear.points[0].inContact = true;
    runtime.propulsion.engines[0].rpm = 100.0f;

    const std::optional<mir::WorldPosition> target =
        mir::WorldPosition::FromMeters(mir::WorldVec3(100.0, 250.0, -40.0));
    REQUIRE(target.has_value());
    const glm::quat rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3{0.0f, 1.0f, 0.0f});

    REQUIRE(mir::ResetAircraft(fixture.world, entity, *target, rotation, 60.0f, 0.5f));

    const mir::RigidBody *rigidBody = fixture.world.GetComponent<mir::RigidBody>(entity);
    REQUIRE(rigidBody != nullptr);
    const mir::BodyHandle body = rigidBody->runtime.body;
    const std::optional<mir::WorldPosition> placed = fixture.Physics().BodyPosition(body);
    REQUIRE(placed.has_value());
    const glm::vec3 meters = FloatMeters(placed->Meters());
    CHECK(meters.x == Catch::Approx(100.0f).margin(1e-3f));
    CHECK(meters.y == Catch::Approx(250.0f).margin(1e-3f));
    CHECK(meters.z == Catch::Approx(-40.0f).margin(1e-3f));
    CHECK(std::abs(glm::dot(fixture.Physics().BodyRotation(body), rotation)) == Catch::Approx(1.0f).margin(1e-4f));

    const glm::vec3 nose = rotation * mir::BodyToLocal(glm::vec3{1.0f, 0.0f, 0.0f});
    CHECK(glm::length(fixture.Physics().BodyLinearVelocity(body) - nose * 60.0f) < 1e-3f);
    CHECK(glm::length(fixture.Physics().BodyAngularVelocity(body)) < 1e-4f);

    CHECK(runtime.actuators.throttle == Catch::Approx(0.5f));
    CHECK(runtime.actuators.flapDetent == 0);
    CHECK(runtime.actuators.trim == 0.0f);
    CHECK(runtime.actuators.elevator == 0.0f);
    CHECK(runtime.actuators.aileron == 0.0f);
    CHECK(runtime.actuators.rudder == 0.0f);
    CHECK(runtime.actuators.flaps == 0.0f);
    CHECK(runtime.actuators.elevatorLag == 0.0f);
    CHECK(runtime.commands.count == 0);
    CHECK(runtime.pendingTouchdowns.count == 0);
    CHECK_FALSE(runtime.gear.points[0].inContact);
    CHECK(runtime.propulsion.engines[0].rpm == Catch::Approx(1700.0f));

    CHECK(mir::IsCameraCut(fixture.world, active));
    CHECK_FALSE(mir::IsCameraCut(fixture.world, inactive));
}

TEST_CASE("ResetAircraft refuses an entity without Aircraft", "[flight][telemetry][reset]")
{
    TelemetryFixture fixture;
    fixture.Start();
    const mir::Entity plain = fixture.world.CreateEntity();
    mir::AddTransform(fixture.world, plain, mir::Transform{mir::WorldVec3(0.0)});
    fixture.world.AddComponent<mir::RigidBody>(plain, mir::RigidBody{.motion = mir::PhysicsMotion::Dynamic,
                                                                      .shape = mir::PhysicsShape::Box,
                                                                      .allowSleeping = false});
    fixture.Run(1);

    const std::optional<mir::WorldPosition> position = mir::WorldPosition::FromMeters(mir::WorldVec3(0.0));
    REQUIRE(position.has_value());
    CHECK_FALSE(mir::ResetAircraft(fixture.world, plain, *position, mir::kIdentityRotation, 50.0f, 0.5f));
}
