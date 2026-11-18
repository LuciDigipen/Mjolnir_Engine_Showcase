/**
 * @file AircraftConfig.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight/AircraftConfig, AircraftConfigStore and AircraftConfigSystem.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/AircraftConfig.h>
#include <flight/AircraftConfigStore.h>
#include <flight/AircraftConfigSystem.h>
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

#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr float kStep = 1.0f / 60.0f;

    std::string_view kValidConfig = R"({
        "version": 1,
        "mass": 1200.0,
        "inertia": { "diagonal": [1000.0, 2000.0, 2500.0], "products": [0.0, 150.0, 0.0] },
        "centerOfMass": [0.5, 0.0, 0.2],
        "reference": { "area": 16.0, "span": 11.0, "chord": 1.5 },
        "camera": { "eye": [0.8, 0.0, -0.4], "chaseDistance": 12.0, "chaseHeight": 3.0 },
        "nodes": { "aileronLeft": "Aileron_L", "elevator": "Elevator" },
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
        "stallWarningMarginDeg": 3.0,
        "aero": { "L1": { "CL": 0.4, "CD": 0.04, "Ka": 3.0, "Ke": 4.0, "Kr": 2.0,
                           "Kp": 2000.0, "Kq": 4000.0, "Kn": 2000.0 } }
    })";

    constexpr std::string_view kActuators = R"({
        "elevator": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 60.0, "delay": 0.05 },
        "aileron": { "maxDeflectionDeg": 20.0, "rateDegPerSec": 80.0, "delay": 0.05 },
        "rudder": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 60.0, "delay": 0.05 },
        "flaps": { "detentsDeg": [0.0, 10.0, 20.0, 30.0], "rateDegPerSec": 5.0 },
        "trim": { "stepDeg": 0.5, "maxDeg": 10.0 }
    })";

    std::string ConfigDocument(std::string_view throttleRate, std::string_view actuators)
    {
        std::string document = R"({
            "version": 1, "mass": 10, "inertia": { "diagonal": [1, 2, 3] },
            "centerOfMass": [0, 0, 0], "reference": { "area": 1, "span": 1, "chord": 1 },
            "camera": { "eye": [0, 0, 0], "chaseDistance": 0, "chaseHeight": 0 },
        )";
        document += throttleRate;
        if (actuators.empty())
        {
            while (!document.empty()
                && (document.back() == ' ' || document.back() == '\n' || document.back() == ','))
                document.pop_back();
        }
        else
        {
            document += R"( "actuators": )";
            document += actuators;
        }
        document += "}";
        return document;
    }

    std::string AeroDocument(std::string_view aero)
    {
        std::string document = R"({
            "version": 1, "mass": 10, "inertia": { "diagonal": [1, 2, 3] },
            "centerOfMass": [0, 0, 0], "reference": { "area": 1, "span": 1, "chord": 1 },
            "camera": { "eye": [0, 0, 0], "chaseDistance": 0, "chaseHeight": 0 },
            "throttleRate": 0.5, "actuators": )";
        document += kActuators;
        document += R"(, "aero": )";
        document += aero;
        document += "}";
        return document;
    }

    std::vector<std::byte> RawBlob(std::string_view text)
    {
        const std::span<const std::byte> bytes{reinterpret_cast<const std::byte *>(text.data()), text.size()};
        return mir::BuildAssetBlob(mir::kRawAssetTypeTag, mir::kRawAssetContentVersion, bytes);
    }

    struct RawConfig
    {
        explicit RawConfig(std::string_view json) : raw(RawBlob(json)), view(mir::ParseAssetBlob(raw)) {}

        std::vector<std::byte> raw;
        std::optional<mir::AssetBlobView> view;
    };

    void CheckVector(const glm::vec3 &actual, const glm::vec3 &expected)
    {
        CHECK(actual.x == Catch::Approx(expected.x).margin(1e-4f));
        CHECK(actual.y == Catch::Approx(expected.y).margin(1e-4f));
        CHECK(actual.z == Catch::Approx(expected.z).margin(1e-4f));
    }

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
        mir::SystemContext context{world, commands, kStep, 0.0, 0};
        mir::Registration hooks;

        FlightFixture()
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

        ~FlightFixture()
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

        mir::Entity AddAircraft(std::string_view path)
        {
            const mir::Entity entity = world.CreateEntity();
            mir::AddTransform(world, entity, mir::Transform{mir::WorldVec3{0.0}});
            world.AddComponent<mir::RigidBody>(entity, mir::RigidBody{.motion = mir::PhysicsMotion::Dynamic,
                                                                      .shape = mir::PhysicsShape::Box,
                                                                      .allowSleeping = false});
            mir::Aircraft aircraft;
            SetConfig(aircraft, path);
            world.AddComponent<mir::Aircraft>(entity, aircraft);
            return entity;
        }
    };
}

TEST_CASE("A full aircraft config parses", "[flight][config]")
{
    mir::AircraftConfig config;
    std::string error;

    REQUIRE(mir::ParseAircraftConfig(kValidConfig, config, error));
    CHECK(error.empty());
    CHECK(config.version == 1);
    CHECK(config.mass == Catch::Approx(1200.0f));
    CheckVector(config.inertiaDiagonal, glm::vec3{1000.0f, 2000.0f, 2500.0f});
    CheckVector(config.inertiaProducts, glm::vec3{0.0f, 150.0f, 0.0f});
    CheckVector(config.centerOfMass, glm::vec3{0.5f, 0.0f, 0.2f});
    CHECK(config.reference.area == Catch::Approx(16.0f));
    CHECK(config.reference.span == Catch::Approx(11.0f));
    CHECK(config.reference.chord == Catch::Approx(1.5f));
    CheckVector(config.camera.eye, glm::vec3{0.8f, 0.0f, -0.4f});
    CHECK(config.camera.chaseDistance == Catch::Approx(12.0f));
    CHECK(config.camera.chaseHeight == Catch::Approx(3.0f));
    REQUIRE(config.nodes.size() == 2);
    CHECK(config.nodes[0].component == "aileronLeft");
    CHECK(config.nodes[0].model == "Aileron_L");
    CHECK(config.nodes[1].component == "elevator");
    CHECK(config.nodes[1].model == "Elevator");
    CHECK(config.throttleRate == Catch::Approx(0.5f));
    CHECK(config.actuators.elevator.maxDeflection == Catch::Approx(glm::radians(25.0f)));
    CHECK(config.actuators.elevator.rate == Catch::Approx(glm::radians(60.0f)));
    CHECK(config.actuators.elevator.delay == Catch::Approx(0.05f));
    CHECK(config.actuators.aileron.maxDeflection == Catch::Approx(glm::radians(20.0f)));
    CHECK(config.actuators.aileron.rate == Catch::Approx(glm::radians(80.0f)));
    CHECK(config.actuators.rudder.maxDeflection == Catch::Approx(glm::radians(25.0f)));
    REQUIRE(config.actuators.flaps.detents.size() == 4);
    CHECK(config.actuators.flaps.detents[0] == Catch::Approx(0.0f));
    CHECK(config.actuators.flaps.detents[3] == Catch::Approx(glm::radians(30.0f)));
    CHECK(config.actuators.flaps.rate == Catch::Approx(glm::radians(5.0f)));
    CHECK(config.actuators.trim.step == Catch::Approx(glm::radians(0.5f)));
    CHECK(config.actuators.trim.max == Catch::Approx(glm::radians(10.0f)));
    REQUIRE(config.aero.l1.has_value());
    CHECK(config.aero.l1->CL == Catch::Approx(0.4f));
    CHECK(config.aero.l1->CD == Catch::Approx(0.04f));
    CHECK(config.aero.l1->Ka == Catch::Approx(3.0f));
    CHECK(config.aero.l1->Ke == Catch::Approx(4.0f));
    CHECK(config.aero.l1->Kr == Catch::Approx(2.0f));
    CHECK(config.aero.l1->Kp == Catch::Approx(2000.0f));
    CHECK(config.aero.l1->Kq == Catch::Approx(4000.0f));
    CHECK(config.aero.l1->Kn == Catch::Approx(2000.0f));
}

TEST_CASE("The aerodynamic profile is optional", "[flight][config]")
{
    mir::AircraftConfig config;
    std::string error;

    SECTION("without an aero object")
    {
        REQUIRE(mir::ParseAircraftConfig(ConfigDocument(R"("throttleRate": 0.5,)", kActuators), config, error));
        CHECK_FALSE(config.aero.l1.has_value());
    }

    SECTION("an empty aero object has no tier profile")
    {
        REQUIRE(mir::ParseAircraftConfig(AeroDocument("{}"), config, error));
        CHECK_FALSE(config.aero.l1.has_value());
    }

    SECTION("a negative lift coefficient is accepted")
    {
        REQUIRE(mir::ParseAircraftConfig(
            AeroDocument(R"({ "L1": { "CL": -0.2, "CD": 0, "Ka": 0, "Ke": 0, "Kr": 0, "Kp": 0, "Kq": 0, "Kn": 0 } })"),
            config, error));
        REQUIRE(config.aero.l1.has_value());
        CHECK(config.aero.l1->CL == Catch::Approx(-0.2f));
    }
}

TEST_CASE("Aircraft config errors name the JSON path", "[flight][config]")
{
    mir::AircraftConfig config;
    std::string error;

    SECTION("invalid JSON reports the parser message")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig("{", config, error));
        CHECK(error.find("parse error") != std::string::npos);
    }

    SECTION("unknown key")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(R"({ "masss": 1 })", config, error));
        CHECK(error == "masss: unknown key");
    }

    SECTION("version must be 1")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(R"({ "version": 2 })", config, error));
        CHECK(error == "version: must be 1");
    }

    SECTION("missing inertia diagonal")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(R"({ "version": 1, "mass": 10, "inertia": {} })", config, error));
        CHECK(error == "inertia.diagonal: missing");
    }

    SECTION("inertia diagonal needs three numbers")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(
            R"({ "version": 1, "mass": 10, "inertia": { "diagonal": [1, 2] } })", config, error));
        CHECK(error == "inertia.diagonal: expected 3 numbers");
    }

    SECTION("a non-positive diagonal moment is rejected")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(
            R"({ "version": 1, "mass": 10, "inertia": { "diagonal": [0, 2, 3] } })", config, error));
        CHECK(error == "inertia.diagonal[0]: must be > 0");
    }

    SECTION("missing reference chord")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(R"({
            "version": 1, "mass": 10, "inertia": { "diagonal": [1, 2, 3] },
            "centerOfMass": [0, 0, 0], "reference": { "area": 1, "span": 1 }
        })",
            config, error));
        CHECK(error == "reference.chord: missing");
    }

    SECTION("a negative chase distance is rejected")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(R"({
            "version": 1, "mass": 10, "inertia": { "diagonal": [1, 2, 3] },
            "centerOfMass": [0, 0, 0], "reference": { "area": 1, "span": 1, "chord": 1 },
            "camera": { "eye": [0, 0, 0], "chaseDistance": -1, "chaseHeight": 0 }
        })",
            config, error));
        CHECK(error == "camera.chaseDistance: must be >= 0");
    }

    SECTION("a number that does not fit a float is rejected")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(R"({
            "version": 1, "mass": 1e39, "inertia": { "diagonal": [1, 2, 3] }
        })",
            config, error));
        CHECK(error == "mass: must be finite");
    }

    SECTION("missing throttleRate")
    {
        const std::string document = ConfigDocument("", kActuators);
        REQUIRE_FALSE(mir::ParseAircraftConfig(document, config, error));
        CHECK(error == "throttleRate: missing");
    }

    SECTION("a non-positive throttleRate is rejected")
    {
        const std::string document = ConfigDocument(R"("throttleRate": 0,)", kActuators);
        REQUIRE_FALSE(mir::ParseAircraftConfig(document, config, error));
        CHECK(error == "throttleRate: must be > 0");
    }

    SECTION("missing actuators")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(ConfigDocument(R"("throttleRate": 0.5,)", ""), config, error));
        CHECK(error == "actuators: missing");
    }

    SECTION("an elevator without a delay is rejected")
    {
        const std::string document = ConfigDocument(R"("throttleRate": 0.5,)",
            R"({
                "elevator": { "maxDeflectionDeg": 25, "rateDegPerSec": 60 },
                "aileron": { "maxDeflectionDeg": 20, "rateDegPerSec": 80, "delay": 0 },
                "rudder": { "maxDeflectionDeg": 25, "rateDegPerSec": 60, "delay": 0 },
                "flaps": { "detentsDeg": [0], "rateDegPerSec": 5 },
                "trim": { "stepDeg": 0.5, "maxDeg": 10 }
            })");
        REQUIRE_FALSE(mir::ParseAircraftConfig(document, config, error));
        CHECK(error == "actuators.elevator.delay: missing");
    }

    SECTION("a non-positive surface rate is rejected")
    {
        const std::string document = ConfigDocument(R"("throttleRate": 0.5,)",
            R"({
                "elevator": { "maxDeflectionDeg": 25, "rateDegPerSec": 60, "delay": 0 },
                "aileron": { "maxDeflectionDeg": 20, "rateDegPerSec": 0, "delay": 0 },
                "rudder": { "maxDeflectionDeg": 25, "rateDegPerSec": 60, "delay": 0 },
                "flaps": { "detentsDeg": [0], "rateDegPerSec": 5 },
                "trim": { "stepDeg": 0.5, "maxDeg": 10 }
            })");
        REQUIRE_FALSE(mir::ParseAircraftConfig(document, config, error));
        CHECK(error == "actuators.aileron.rateDegPerSec: must be > 0");
    }

    SECTION("an empty flap detent list is rejected")
    {
        const std::string document = ConfigDocument(R"("throttleRate": 0.5,)",
            R"({
                "elevator": { "maxDeflectionDeg": 25, "rateDegPerSec": 60, "delay": 0 },
                "aileron": { "maxDeflectionDeg": 20, "rateDegPerSec": 80, "delay": 0 },
                "rudder": { "maxDeflectionDeg": 25, "rateDegPerSec": 60, "delay": 0 },
                "flaps": { "detentsDeg": [], "rateDegPerSec": 5 },
                "trim": { "stepDeg": 0.5, "maxDeg": 10 }
            })");
        REQUIRE_FALSE(mir::ParseAircraftConfig(document, config, error));
        CHECK(error == "actuators.flaps.detentsDeg: must not be empty");
    }

    SECTION("flap detents must be strictly increasing")
    {
        const std::string document = ConfigDocument(R"("throttleRate": 0.5,)",
            R"({
                "elevator": { "maxDeflectionDeg": 25, "rateDegPerSec": 60, "delay": 0 },
                "aileron": { "maxDeflectionDeg": 20, "rateDegPerSec": 80, "delay": 0 },
                "rudder": { "maxDeflectionDeg": 25, "rateDegPerSec": 60, "delay": 0 },
                "flaps": { "detentsDeg": [0, 10, 10], "rateDegPerSec": 5 },
                "trim": { "stepDeg": 0.5, "maxDeg": 10 }
            })");
        REQUIRE_FALSE(mir::ParseAircraftConfig(document, config, error));
        CHECK(error == "actuators.flaps.detentsDeg: must be strictly increasing");
    }

    SECTION("a negative trim range is rejected")
    {
        const std::string document = ConfigDocument(R"("throttleRate": 0.5,)",
            R"({
                "elevator": { "maxDeflectionDeg": 25, "rateDegPerSec": 60, "delay": 0 },
                "aileron": { "maxDeflectionDeg": 20, "rateDegPerSec": 80, "delay": 0 },
                "rudder": { "maxDeflectionDeg": 25, "rateDegPerSec": 60, "delay": 0 },
                "flaps": { "detentsDeg": [0], "rateDegPerSec": 5 },
                "trim": { "stepDeg": 0.5, "maxDeg": -1 }
            })");
        REQUIRE_FALSE(mir::ParseAircraftConfig(document, config, error));
        CHECK(error == "actuators.trim.maxDeg: must be >= 0");
    }

    SECTION("aero must be an object")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(AeroDocument("3"), config, error));
        CHECK(error == "aero: expected an object");
    }

    SECTION("aero.L1 must be an object")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(AeroDocument(R"({ "L1": 3 })"), config, error));
        CHECK(error == "aero.L1: expected an object");
    }

    SECTION("a missing drag coefficient names its path")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(
            AeroDocument(R"({ "L1": { "CL": 0.4, "Ka": 0, "Ke": 0, "Kr": 0, "Kp": 0, "Kq": 0, "Kn": 0 } })"), config,
            error));
        CHECK(error == "aero.L1.CD: missing");
    }

    SECTION("a negative drag coefficient is rejected")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(
            AeroDocument(R"({ "L1": { "CL": 0.4, "CD": -1, "Ka": 0, "Ke": 0, "Kr": 0, "Kp": 0, "Kq": 0, "Kn": 0 } })"),
            config, error));
        CHECK(error == "aero.L1.CD: must be >= 0");
    }

    SECTION("a negative damping gain is rejected")
    {
        const std::string aero =
            R"({ "L1": { "CL": 0.4, "CD": 0.04, "Ka": 0, "Ke": 0, "Kr": 0, "Kp": 0, "Kq": -1, "Kn": 0 } })";
        REQUIRE_FALSE(mir::ParseAircraftConfig(AeroDocument(aero), config, error));
        CHECK(error == "aero.L1.Kq: must be >= 0");
    }
}

TEST_CASE("The aircraft config decoder reads a raw blob only", "[flight][config]")
{
    mir::AircraftConfig config;
    std::string error;

    const RawConfig valid(kValidConfig);
    REQUIRE(valid.view.has_value());
    const mir::AssetDependencies none{[](mir::AssetId) -> const mir::AssetBlobView * { return nullptr; }};
    CHECK(mir::AircraftConfigDecoder().decode(*valid.view, none, config, error));
    CHECK(config.mass == Catch::Approx(1200.0f));

    const std::span<const std::byte> content{
        reinterpret_cast<const std::byte *>(kValidConfig.data()), kValidConfig.size()};
    const std::vector<std::byte> wrong = mir::BuildAssetBlob(0xDEADBEEFu, 1u, content);
    const std::optional<mir::AssetBlobView> wrongView = mir::ParseAssetBlob(wrong);
    REQUIRE(wrongView.has_value());
    error.clear();
    CHECK_FALSE(mir::AircraftConfigDecoder().decode(*wrongView, none, config, error));
    CHECK(error.find("0xdeadbeef") != std::string::npos);
}

TEST_CASE("The aircraft config store decodes each path once", "[flight][config]")
{
    const RawConfig raw(kValidConfig);
    REQUIRE(raw.view.has_value());

    int loads = 0;
    mir::AircraftConfigStore store(
        [&raw, &loads](std::string_view, std::string &) -> const mir::AssetBlobView *
        {
            ++loads;
            return &*raw.view;
        });

    const mir::AircraftConfig *first = store.Get("plane.aircraft.json");
    REQUIRE(first != nullptr);
    CHECK(first->mass == Catch::Approx(1200.0f));
    const mir::AircraftConfig *second = store.Get("plane.aircraft.json");
    CHECK(second == first);
    CHECK(loads == 1);
}

TEST_CASE("The aircraft config store caches a failure", "[flight][config]")
{
    int loads = 0;
    mir::AircraftConfigStore store(
        [&loads](std::string_view, std::string &error) -> const mir::AssetBlobView *
        {
            ++loads;
            error = "not found";
            return nullptr;
        });

    CHECK(store.Get("missing.aircraft.json") == nullptr);
    CHECK(store.Get("missing.aircraft.json") == nullptr);
    CHECK(loads == 1);
}

TEST_CASE("WriteMassProperties moves the tensor and centre of mass to local axes", "[flight][config]")
{
    mir::AircraftConfig config;
    std::string error;
    REQUIRE(mir::ParseAircraftConfig(kValidConfig, config, error));

    mir::RigidBody rigidBody;
    mir::WriteMassProperties(config, rigidBody);

    CHECK(rigidBody.mass == Catch::Approx(1200.0f));
    CHECK(rigidBody.customInertia);
    CheckVector(rigidBody.inertiaDiagonal, glm::vec3{2000.0f, 2500.0f, 1000.0f});
    CheckVector(rigidBody.inertiaProducts, glm::vec3{0.0f, 0.0f, 150.0f});
    CHECK(rigidBody.customCenterOfMass);
    CheckVector(rigidBody.centerOfMassOffset, glm::vec3{0.0f, -0.2f, -0.5f});
    CHECK(rigidBody.linearDamping == 0.0f);
    CHECK(rigidBody.angularDamping == 0.0f);
    CHECK(rigidBody.gyroscopicForce);
    CHECK(rigidBody.motionQuality == mir::PhysicsMotionQuality::LinearCast);
}

TEST_CASE("An aircraft takes its mass properties from its config", "[flight][config]")
{
    const RawConfig plane(kValidConfig);
    const RawConfig heavy(R"({
        "version": 1,
        "mass": 2000.0,
        "inertia": { "diagonal": [3000.0, 4000.0, 5000.0] },
        "centerOfMass": [1.0, 0.0, 0.0],
        "reference": { "area": 20.0, "span": 12.0, "chord": 2.0 },
        "camera": { "eye": [1.0, 0.0, 0.0], "chaseDistance": 14.0, "chaseHeight": 4.0 },
        "throttleRate": 0.5,
        "actuators": {
            "elevator": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 60.0, "delay": 0.05 },
            "aileron": { "maxDeflectionDeg": 20.0, "rateDegPerSec": 80.0, "delay": 0.05 },
            "rudder": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 60.0, "delay": 0.05 },
            "flaps": { "detentsDeg": [0.0, 10.0, 20.0, 30.0], "rateDegPerSec": 5.0 },
            "trim": { "stepDeg": 0.5, "maxDeg": 10.0 }
        }
    })");
    REQUIRE(plane.view.has_value());
    REQUIRE(heavy.view.has_value());

    auto source = [&plane, &heavy](std::string_view path, std::string &error) -> const mir::AssetBlobView *
    {
        if (path == "plane.aircraft.json")
            return &*plane.view;
        if (path == "heavy.aircraft.json")
            return &*heavy.view;
        error = "no asset has the path '" + std::string(path) + "'";
        return nullptr;
    };

    FlightFixture fixture;
    mir::FlightModule flight(fixture.ModuleContext(), source);
    fixture.Start();

    const mir::Entity entity = fixture.AddAircraft("plane.aircraft.json");
    fixture.Run(1);

    const mir::RigidBody *rigidBody = fixture.world.GetComponent<mir::RigidBody>(entity);
    REQUIRE(rigidBody != nullptr);
    const mir::Aircraft *aircraft = fixture.world.GetComponent<mir::Aircraft>(entity);
    REQUIRE(aircraft != nullptr);
    REQUIRE(aircraft->runtime.config != nullptr);
    CHECK(rigidBody->mass == Catch::Approx(1200.0f));
    CheckVector(rigidBody->inertiaDiagonal, glm::vec3{2000.0f, 2500.0f, 1000.0f});
    CheckVector(rigidBody->inertiaProducts, glm::vec3{0.0f, 0.0f, 150.0f});
    CheckVector(rigidBody->centerOfMassOffset, glm::vec3{0.0f, -0.2f, -0.5f});
    CHECK(rigidBody->motion == mir::PhysicsMotion::Dynamic);
    CHECK(fixture.Physics().BodyLinearDamping(rigidBody->runtime.body) == Catch::Approx(0.0f));
    CHECK(fixture.Physics().BodyAngularDamping(rigidBody->runtime.body) == Catch::Approx(0.0f));

    SECTION("a new config path re-applies")
    {
        mir::Aircraft *editable = fixture.world.GetComponent<mir::Aircraft>(entity);
        REQUIRE(editable != nullptr);
        SetConfig(*editable, "heavy.aircraft.json");
        fixture.Run(1);

        const mir::RigidBody *reloaded = fixture.world.GetComponent<mir::RigidBody>(entity);
        REQUIRE(reloaded != nullptr);
        CHECK(reloaded->mass == Catch::Approx(2000.0f));
        CheckVector(reloaded->inertiaDiagonal, glm::vec3{4000.0f, 5000.0f, 3000.0f});
        CheckVector(reloaded->centerOfMassOffset, glm::vec3{0.0f, 0.0f, -1.0f});
        CHECK(fixture.world.GetComponent<mir::Aircraft>(entity)->runtime.config != nullptr);
    }

    SECTION("an unknown path leaves the body untouched")
    {
        mir::Aircraft *editable = fixture.world.GetComponent<mir::Aircraft>(entity);
        REQUIRE(editable != nullptr);
        SetConfig(*editable, "missing.aircraft.json");
        fixture.Run(1);

        const mir::RigidBody *unchanged = fixture.world.GetComponent<mir::RigidBody>(entity);
        REQUIRE(unchanged != nullptr);
        CHECK(unchanged->mass == Catch::Approx(1200.0f));
        CheckVector(unchanged->inertiaDiagonal, glm::vec3{2000.0f, 2500.0f, 1000.0f});
        CheckVector(unchanged->inertiaProducts, glm::vec3{0.0f, 0.0f, 150.0f});
        CheckVector(unchanged->centerOfMassOffset, glm::vec3{0.0f, -0.2f, -0.5f});
        CHECK(fixture.world.GetComponent<mir::Aircraft>(entity)->runtime.config == nullptr);
    }
}
