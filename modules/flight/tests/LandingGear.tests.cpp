/**
 * @file LandingGear.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight/FlightLandingGear and the touchdown events.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/AircraftConfig.h>
#include <flight/AircraftRuntime.h>
#include <flight/FlightFrames.h>
#include <flight/FlightLandingGear.h>
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
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr float kStep = 1.0f / 60.0f;
    constexpr float kRestLength = 0.6f;
    constexpr std::string_view kConfigPath = "gear.aircraft.json";

    constexpr std::string_view kGearPoints = R"([
        { "name": "nose", "mount": [1.5, 0.0, 0.0], "restLength": 0.6, "stiffness": 40000.0, "damping": 10000.0,
          "rollingFriction": 0.03, "sideFriction": 0.9, "brake": 0.5, "maxSteerDeg": 30.0 },
        { "name": "left", "mount": [-0.75, -1.2, 0.0], "restLength": 0.6, "stiffness": 40000.0, "damping": 10000.0,
          "rollingFriction": 0.03, "sideFriction": 0.9, "brake": 0.5, "maxSteerDeg": 0.0 },
        { "name": "right", "mount": [-0.75, 1.2, 0.0], "restLength": 0.6, "stiffness": 40000.0, "damping": 10000.0,
          "rollingFriction": 0.03, "sideFriction": 0.9, "brake": 0.5, "maxSteerDeg": 0.0 }
    ])";

    std::string AircraftDocument(std::string_view gear)
    {
        std::string document = R"({
            "version": 1,
            "mass": 1000.0,
            "inertia": { "diagonal": [2000.0, 3500.0, 2500.0] },
            "centerOfMass": [0.0, 0.0, 0.15],
            "reference": { "area": 16.0, "span": 11.0, "chord": 1.5 },
            "camera": { "eye": [0.8, 0.0, -0.4], "chaseDistance": 12.0, "chaseHeight": 3.0 },
            "throttleRate": 0.5,
            "actuators": {
                "elevator": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 600.0, "delay": 0.0 },
                "aileron": { "maxDeflectionDeg": 20.0, "rateDegPerSec": 600.0, "delay": 0.0 },
                "rudder": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 600.0, "delay": 0.0 },
                "flaps": { "detentsDeg": [0.0], "rateDegPerSec": 5.0 },
                "trim": { "stepDeg": 0.5, "maxDeg": 10.0 }
            },
            "engines": [],
            "gear": )";
        document += gear;
        document += "}";
        return document;
    }

    std::string GearPoint(std::string_view stiffness = "10000", std::string_view damping = "1000",
        std::string_view maxSteerDeg = "0")
    {
        std::string gear = R"({ "name": "wheel", "mount": [0, 0, 0], "restLength": 0.6, "stiffness": )";
        gear += stiffness;
        gear += R"(, "damping": )";
        gear += damping;
        gear += R"(, "rollingFriction": 0.02, "sideFriction": 0.5, "brake": 0, "maxSteerDeg": )";
        gear += maxSteerDeg;
        gear += " }";
        return gear;
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

    void SetConfig(mir::Aircraft &aircraft, std::string_view path)
    {
        std::memset(aircraft.config, 0, sizeof(aircraft.config));
        if (!path.empty())
            std::memcpy(aircraft.config, path.data(), std::min(path.size(), sizeof(aircraft.config) - 1));
    }

    struct GearFixture
    {
        mir::World world;
        mir::CommandBuffer commands;
        mir::SystemScheduler scheduler;
        mir::SystemContext context{world, commands, kStep, 0.0, 0};
        mir::Registration hooks;

        GearFixture()
        {
            mir::RegisterPhysicsComponents();
            world.EmplaceResource<mir::PhysicsWorld>(
                mir::PhysicsWorldDesc{.mGravity = glm::vec3{0.0f, -9.81f, 0.0f}});
            hooks = world.SetComponentHooks<mir::RigidBody>(
                {.onAdd = &mir::PhysicsSystem::OnRigidBodyAdded, .onRemove = &mir::PhysicsSystem::OnRigidBodyRemoved});
            world.EmplaceResource<mir::EngineModeState>();
            mir::SetEngineMode(world, mir::EngineMode::Play);
            mir::SetPlayState(world, mir::PlayState::Running);
            world.Resource<mir::PhysicsWorld>().SetSimulationLive(true);

            scheduler.AddSystem<mir::PhysicsSystem>(mir::SystemPhase::FixedUpdate,
                mir::SystemOrder{mir::SystemOrder::kNormal});
        }

        ~GearFixture()
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

        mir::Entity AddAircraft(double height)
        {
            const mir::Entity entity = world.CreateEntity();
            mir::AddTransform(world, entity, mir::Transform{mir::WorldVec3(0.0, height, 0.0)});
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

    struct ConfigSource
    {
        const mir::AssetBlobView *view = nullptr;

        const mir::AssetBlobView *operator()(std::string_view path, std::string &) const
        {
            return path == kConfigPath ? view : nullptr;
        }
    };

    struct RollResult
    {
        float speed = 0.0f;
        double distance = 0.0;
    };

    RollResult RunForwardCase(const RawConfig &config, bool brake)
    {
        GearFixture fixture;
        mir::FlightModule flight(fixture.ModuleContext(), ConfigSource{&*config.view});
        fixture.Start();
        fixture.AddGround();
        const mir::Entity entity = fixture.AddAircraft(kRestLength + 0.05);
        fixture.Run(1);

        const mir::RigidBody *rigidBody = fixture.world.GetComponent<mir::RigidBody>(entity);
        mir::Aircraft *aircraft = fixture.world.GetComponent<mir::Aircraft>(entity);
        if (rigidBody == nullptr || aircraft == nullptr)
            return {};

        const mir::BodyHandle body = rigidBody->runtime.body;
        const std::optional<mir::WorldPosition> start = fixture.Physics().BodyPosition(body);
        if (!start || !fixture.Physics().SetBodyLinearVelocity(body, glm::vec3{0.0f, 0.0f, -10.0f}))
            return {};
        aircraft->runtime.controls.brake = brake;

        fixture.Run(300);

        const std::optional<mir::WorldPosition> end = fixture.Physics().BodyPosition(body);
        if (!end)
            return {};
        return {glm::length(fixture.Physics().BodyLinearVelocity(body)),
            std::abs(end->Meters().z - start->Meters().z)};
    }

    float YawRateForRudder(const RawConfig &config, float rudder)
    {
        GearFixture fixture;
        mir::FlightModule flight(fixture.ModuleContext(), ConfigSource{&*config.view});
        fixture.Start();
        fixture.AddGround();
        const mir::Entity entity = fixture.AddAircraft(kRestLength + 0.05);
        fixture.Run(1);

        const mir::RigidBody *rigidBody = fixture.world.GetComponent<mir::RigidBody>(entity);
        mir::Aircraft *aircraft = fixture.world.GetComponent<mir::Aircraft>(entity);
        if (rigidBody == nullptr || aircraft == nullptr)
            return 0.0f;
        const mir::BodyHandle body = rigidBody->runtime.body;
        if (!fixture.Physics().SetBodyLinearVelocity(body, glm::vec3{0.0f, 0.0f, -5.0f}))
            return 0.0f;
        aircraft->runtime.controls.yaw = rudder;

        fixture.Run(30);
        return fixture.world.GetComponent<mir::Aircraft>(entity)->runtime.state.angularVelocityBody.z;
    }
}

TEST_CASE("An aircraft settles on its gear and stays settled", "[flight][gear]")
{
    const RawConfig config(AircraftDocument(kGearPoints));
    REQUIRE(config.view.has_value());

    GearFixture fixture;
    mir::FlightModule flight(fixture.ModuleContext(), ConfigSource{&*config.view});
    fixture.Start();
    fixture.AddGround();
    const mir::Entity entity = fixture.AddAircraft(kRestLength + 0.3);

    fixture.Run(180);

    const mir::RigidBody *rigidBody = fixture.world.GetComponent<mir::RigidBody>(entity);
    REQUIRE(rigidBody != nullptr);
    const mir::BodyHandle body = rigidBody->runtime.body;
    CHECK(std::abs(fixture.Physics().BodyLinearVelocity(body).y) < 0.05f);

    const glm::quat rotation = fixture.Physics().BodyRotation(body);
    const glm::vec3 nose = rotation * mir::BodyToLocal(glm::vec3{1.0f, 0.0f, 0.0f});
    const glm::vec3 rightWing = rotation * mir::BodyToLocal(glm::vec3{0.0f, 1.0f, 0.0f});
    CHECK(std::abs(nose.y) < std::sin(glm::radians(1.0f)));
    CHECK(std::abs(rightWing.y) < std::sin(glm::radians(1.0f)));

    const mir::Aircraft *settled = fixture.world.GetComponent<mir::Aircraft>(entity);
    REQUIRE(settled != nullptr);
    for (std::size_t i = 0; i < 3; ++i)
    {
        CHECK(settled->runtime.gear.points[i].inContact);
        CHECK(settled->runtime.gear.points[i].normalForce > 0.0f);
    }

    float fastest = 0.0f;
    for (int i = 0; i < 120; ++i)
    {
        fixture.Run(1);
        fastest = std::max(fastest, std::abs(fixture.Physics().BodyLinearVelocity(body).y));
    }
    CHECK(fastest < 0.05f);

    const mir::Aircraft *stillSettled = fixture.world.GetComponent<mir::Aircraft>(entity);
    REQUIRE(stillSettled != nullptr);
    for (std::size_t i = 0; i < 3; ++i)
        CHECK(stillSettled->runtime.gear.points[i].inContact);
}

TEST_CASE("Gear rays pass through the aircraft's own body", "[flight][gear]")
{
    const RawConfig config(AircraftDocument(kGearPoints));
    REQUIRE(config.view.has_value());

    GearFixture fixture;
    mir::FlightModule flight(fixture.ModuleContext(), ConfigSource{&*config.view});
    fixture.Start();
    fixture.AddGround();
    const mir::Entity entity = fixture.AddAircraft(5.0);

    fixture.Run(1);

    const mir::Aircraft *aircraft = fixture.world.GetComponent<mir::Aircraft>(entity);
    REQUIRE(aircraft != nullptr);
    for (std::size_t i = 0; i < 3; ++i)
        CHECK_FALSE(aircraft->runtime.gear.points[i].inContact);

    const mir::RigidBody *rigidBody = fixture.world.GetComponent<mir::RigidBody>(entity);
    REQUIRE(rigidBody != nullptr);
    const mir::BodyHandle body = rigidBody->runtime.body;
    const std::optional<mir::WorldPosition> position = fixture.Physics().BodyPosition(body);
    REQUIRE(position.has_value());
    const std::optional<mir::WorldPosition> mount = mir::WorldPosition::FromMeters(
        position->Meters() + mir::WorldVec3(mir::BodyToLocal(glm::vec3{1.5f, 0.0f, 0.0f})));
    REQUIRE(mount.has_value());

    const std::optional<mir::RayHit> own = fixture.Physics().RayCast(*mount,
        mir::BodyToLocal(glm::vec3{0.0f, 0.0f, 1.0f}), kRestLength);
    REQUIRE(own.has_value());
    CHECK(own->mBody == body);
}

TEST_CASE("Held brakes stop the aircraft and free rolling travels farther", "[flight][gear]")
{
    const RawConfig config(AircraftDocument(kGearPoints));
    REQUIRE(config.view.has_value());

    const RollResult braked = RunForwardCase(config, true);
    const RollResult rolling = RunForwardCase(config, false);

    CHECK(braked.speed < 0.1f);
    CHECK(rolling.speed > 1.0f);
    CHECK(braked.distance < rolling.distance);
}

TEST_CASE("The nose wheel follows the rudder", "[flight][gear]")
{
    const RawConfig config(AircraftDocument(kGearPoints));
    REQUIRE(config.view.has_value());

    SECTION("a positive rudder yaws right")
    {
        CHECK(YawRateForRudder(config, 1.0f) > 0.05f);
    }

    SECTION("a negative rudder yaws left")
    {
        CHECK(YawRateForRudder(config, -1.0f) < -0.05f);
    }
}

TEST_CASE("A touchdown publishes one event per gear", "[flight][gear]")
{
    const RawConfig config(AircraftDocument(kGearPoints));
    REQUIRE(config.view.has_value());

    GearFixture fixture;
    mir::FlightModule flight(fixture.ModuleContext(), ConfigSource{&*config.view});
    fixture.Start();
    fixture.AddGround();
    const mir::Entity entity = fixture.AddAircraft(kRestLength + 0.02);
    fixture.Run(1);

    const mir::RigidBody *rigidBody = fixture.world.GetComponent<mir::RigidBody>(entity);
    REQUIRE(rigidBody != nullptr);
    REQUIRE(fixture.Physics().SetBodyLinearVelocity(rigidBody->runtime.body, glm::vec3{0.0f, -1.5f, 0.0f}));

    for (int frame = 0; frame < 10
        && fixture.world.GetComponent<mir::Aircraft>(entity)->runtime.touchdowns.count == 0;
        ++frame)
        fixture.Run(1);

    const mir::Aircraft *landed = fixture.world.GetComponent<mir::Aircraft>(entity);
    REQUIRE(landed != nullptr);
    const std::span<const mir::Touchdown> events = mir::AircraftTouchdowns(*landed);
    REQUIRE(events.size() == 3);
    for (std::size_t i = 0; i < events.size(); ++i)
    {
        CHECK(events[i].gearIndex == static_cast<int>(i));
        CHECK(events[i].sinkRate == Catch::Approx(1.5f).margin(0.3f));
        CHECK(events[i].compression > 0.0f);
    }

    for (int frame = 0; frame < 120; ++frame)
    {
        fixture.Run(1);
        CHECK(fixture.world.GetComponent<mir::Aircraft>(entity)->runtime.touchdowns.count == 0);
    }
}

TEST_CASE("A config without gear applies no gear force", "[flight][gear]")
{
    const RawConfig config(AircraftDocument("[]"));
    REQUIRE(config.view.has_value());

    GearFixture fixture;
    mir::FlightModule flight(fixture.ModuleContext(), ConfigSource{&*config.view});
    fixture.Start();
    fixture.AddGround();
    const mir::Entity entity = fixture.AddAircraft(3.0);

    fixture.Run(30);

    const mir::RigidBody *rigidBody = fixture.world.GetComponent<mir::RigidBody>(entity);
    REQUIRE(rigidBody != nullptr);
    CHECK(fixture.Physics().BodyLinearVelocity(rigidBody->runtime.body).y < -1.0f);

    const mir::Aircraft *aircraft = fixture.world.GetComponent<mir::Aircraft>(entity);
    REQUIRE(aircraft != nullptr);
    for (std::size_t i = 0; i < mir::kMaxGear; ++i)
    {
        CHECK_FALSE(aircraft->runtime.gear.points[i].inContact);
        CHECK(aircraft->runtime.gear.points[i].normalForce == 0.0f);
    }
    CHECK(aircraft->runtime.accumulator.force == glm::vec3{0.0f});
}

TEST_CASE("Gear parses from the config", "[flight][gear][config]")
{
    mir::AircraftConfig config;
    std::string error;

    SECTION("a full gear list")
    {
        REQUIRE(mir::ParseAircraftConfig(AircraftDocument(kGearPoints), config, error));
        REQUIRE(config.gear.size() == 3);
        const mir::AircraftConfig::Gear &nose = config.gear[0];
        CHECK(nose.name == "nose");
        CHECK(nose.mount == glm::vec3{1.5f, 0.0f, 0.0f});
        CHECK(nose.restLength == Catch::Approx(kRestLength));
        CHECK(nose.stiffness == Catch::Approx(40000.0f));
        CHECK(nose.damping == Catch::Approx(10000.0f));
        CHECK(nose.rollingFriction == Catch::Approx(0.03f));
        CHECK(nose.sideFriction == Catch::Approx(0.9f));
        CHECK(nose.brake == Catch::Approx(0.5f));
        CHECK(nose.maxSteer == Catch::Approx(glm::radians(30.0f)));
        CHECK(config.gear[1].maxSteer == Catch::Approx(0.0f));
    }

    SECTION("missing gear")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(R"({
            "version": 1, "mass": 10, "inertia": { "diagonal": [1, 2, 3] },
            "centerOfMass": [0, 0, 0], "reference": { "area": 1, "span": 1, "chord": 1 },
            "camera": { "eye": [0, 0, 0], "chaseDistance": 0, "chaseHeight": 0 },
            "throttleRate": 0.5,
            "actuators": {
                "elevator": { "maxDeflectionDeg": 25, "rateDegPerSec": 60, "delay": 0 },
                "aileron": { "maxDeflectionDeg": 20, "rateDegPerSec": 80, "delay": 0 },
                "rudder": { "maxDeflectionDeg": 25, "rateDegPerSec": 60, "delay": 0 },
                "flaps": { "detentsDeg": [0], "rateDegPerSec": 5 },
                "trim": { "stepDeg": 0.5, "maxDeg": 10 }
            },
            "engines": []
        })",
            config, error));
        CHECK(error == "gear: missing");
    }

    SECTION("gear must be an array")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(AircraftDocument("3"), config, error));
        CHECK(error == "gear: expected an array");
    }

    SECTION("at most eight gear points")
    {
        std::string gear = "[";
        for (int i = 0; i < 9; ++i)
        {
            if (i > 0)
                gear += ", ";
            gear += GearPoint();
        }
        gear += "]";

        REQUIRE_FALSE(mir::ParseAircraftConfig(AircraftDocument(gear), config, error));
        CHECK(error == "gear: at most 8 gear points");
    }

    SECTION("a gear point must be an object")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(AircraftDocument("[3]"), config, error));
        CHECK(error == "gear[0]: expected an object");
    }

    SECTION("a name is required")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(AircraftDocument(R"([{ "mount": [0, 0, 0], "restLength": 0.6,
            "stiffness": 10000, "damping": 1000, "rollingFriction": 0.02, "sideFriction": 0.5, "brake": 0,
            "maxSteerDeg": 0 }])"),
            config, error));
        CHECK(error == "gear[0].name: missing");
    }

    SECTION("a non-positive stiffness is rejected")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(AircraftDocument("[" + GearPoint("0") + "]"), config, error));
        CHECK(error == "gear[0].stiffness: must be > 0");
    }

    SECTION("a negative damping is rejected")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(AircraftDocument("[" + GearPoint("10000", "-1") + "]"), config,
            error));
        CHECK(error == "gear[0].damping: must be >= 0");
    }

    SECTION("a negative steer limit is rejected")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(AircraftDocument("[" + GearPoint("10000", "1000", "-1") + "]"),
            config, error));
        CHECK(error == "gear[0].maxSteerDeg: must be >= 0");
    }
}

