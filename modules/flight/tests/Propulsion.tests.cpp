/**
 * @file Propulsion.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight/FlightPropulsion.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/AircraftConfig.h>
#include <flight/AircraftRuntime.h>
#include <flight/FlightModule.h>
#include <flight/FlightPropulsion.h>
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
    constexpr float kIdleRpm = 1000.0f;
    constexpr float kMaxRpm = 3000.0f;
    constexpr float kRpmTau = 0.8f;
    constexpr float kSeaLevelDensity = 1.225f;

    mir::AircraftConfig SingleEngine()
    {
        mir::AircraftConfig config;
        mir::AircraftConfig::Engine engine;
        engine.position = glm::vec3{1.5f, 0.0f, 0.0f};
        engine.direction = glm::vec3{1.0f, 0.0f, 0.0f};
        engine.idleRpm = kIdleRpm;
        engine.maxRpm = kMaxRpm;
        engine.rpmTau = kRpmTau;
        engine.staticThrust = {{0.0f, 0.0f}, {1.0f, 4000.0f}};
        engine.speedFactor = {{0.0f, 1.0f}, {60.0f, 0.5f}, {120.0f, 0.3f}};
        config.engines.push_back(engine);
        return config;
    }

    mir::AircraftRuntime StationaryAir()
    {
        mir::AircraftRuntime runtime;
        runtime.airData.density = kSeaLevelDensity;
        runtime.airData.relativeVelocityBody = glm::vec3{0.0f};
        return runtime;
    }

    float ThrustAt(const mir::AircraftConfig &config, const glm::vec3 &velocity, float density)
    {
        mir::AircraftRuntime runtime = StationaryAir();
        runtime.actuators.throttle = 1.0f;
        mir::StepPropulsion(config, runtime, 0.0f);
        runtime.propulsion.engines[0].rpm = kMaxRpm;
        runtime.airData.relativeVelocityBody = velocity;
        runtime.airData.density = density;
        mir::StepPropulsion(config, runtime, 0.0f);
        return runtime.propulsion.engines[0].thrust;
    }

    constexpr std::string_view kActuators = R"({
        "elevator": { "maxDeflectionDeg": 25, "rateDegPerSec": 60, "delay": 0 },
        "aileron": { "maxDeflectionDeg": 20, "rateDegPerSec": 80, "delay": 0 },
        "rudder": { "maxDeflectionDeg": 25, "rateDegPerSec": 60, "delay": 0 },
        "flaps": { "detentsDeg": [0], "rateDegPerSec": 5 },
        "trim": { "stepDeg": 0.5, "maxDeg": 10 }
    })";

    std::string BaseDocument()
    {
        std::string document = R"({
            "version": 1, "mass": 10, "inertia": { "diagonal": [1, 2, 3] },
            "centerOfMass": [0, 0, 0], "reference": { "area": 1, "span": 1, "chord": 1 },
            "camera": { "eye": [0, 0, 0], "chaseDistance": 0, "chaseHeight": 0 },
            "throttleRate": 0.5, "actuators": )";
        document += kActuators;
        return document;
    }

    std::string EngineJson(std::string_view position = "[1.5, 0, 0]", std::string_view direction = "[1, 0, 0]",
        std::string_view idleRpm = "700", std::string_view maxRpm = "2700", std::string_view rpmTau = "0.8",
        std::string_view staticThrust = "[[0, 0], [1, 3200]]", std::string_view speedFactor = "[[0, 1], [60, 0.8]]")
    {
        std::string engine = R"({ "position": )";
        engine += position;
        engine += R"(, "direction": )";
        engine += direction;
        engine += R"(, "idleRpm": )";
        engine += idleRpm;
        engine += R"(, "maxRpm": )";
        engine += maxRpm;
        engine += R"(, "rpmTau": )";
        engine += rpmTau;
        engine += R"(, "staticThrust": )";
        engine += staticThrust;
        engine += R"(, "speedFactor": )";
        engine += speedFactor;
        engine += " }";
        return engine;
    }

    std::string EngineDocument(std::string_view engines)
    {
        std::string document = BaseDocument();
        document += R"(, "engines": )";
        document += engines;
        document += "}";
        return document;
    }

    constexpr std::string_view kPoweredConfig = R"({
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
        "engines": [{
            "position": [0.0, 0.0, 0.0], "direction": [1.0, 0.0, 0.0],
            "idleRpm": 0.0, "maxRpm": 2700.0, "rpmTau": 0.5,
            "staticThrust": [[0.0, 0.0], [1.0, 3000.0]], "speedFactor": [[0.0, 1.0]]
        }]
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

        mir::Entity AddAircraft(std::string_view path)
        {
            const mir::Entity entity = world.CreateEntity();
            mir::AddTransform(world, entity, mir::Transform{ mir::WorldVec3{0.0} });
            world.AddComponent<mir::RigidBody>(entity, mir::RigidBody{ .motion = mir::PhysicsMotion::Dynamic,
                                                                      .shape = mir::PhysicsShape::Box,
                                                                      .allowSleeping = false });
            mir::Aircraft aircraft;
            SetConfig(aircraft, path);
            world.AddComponent<mir::Aircraft>(entity, aircraft);
            return entity;
        }
    };
}

TEST_CASE("Engine rpm follows the throttle with a first-order lag", "[flight][propulsion]")
{
    const mir::AircraftConfig config = SingleEngine();
    mir::AircraftRuntime runtime = StationaryAir();
    runtime.actuators.throttle = 1.0f;

    mir::StepPropulsion(config, runtime, kRpmTau);
    const float lag = 1.0f - std::exp(-1.0f);
    CHECK(runtime.propulsion.engines[0].rpm == Catch::Approx(kIdleRpm + lag * (kMaxRpm - kIdleRpm)).epsilon(0.01f));

    for (int i = 0; i < 100; ++i)
        mir::StepPropulsion(config, runtime, kRpmTau);
    CHECK(runtime.propulsion.engines[0].rpm == Catch::Approx(kMaxRpm).epsilon(1e-4f));

    runtime.actuators.throttle = 0.0f;
    mir::StepPropulsion(config, runtime, kRpmTau);
    CHECK(runtime.propulsion.engines[0].rpm
        == Catch::Approx(kIdleRpm + (kMaxRpm - kIdleRpm) * std::exp(-1.0f)).epsilon(0.01f));

    for (int i = 0; i < 100; ++i)
        mir::StepPropulsion(config, runtime, kRpmTau);
    CHECK(runtime.propulsion.engines[0].rpm == Catch::Approx(kIdleRpm).epsilon(1e-4f));
}

TEST_CASE("A new config resets the engine rpm to its idle speed", "[flight][propulsion]")
{
    const mir::AircraftConfig first = SingleEngine();
    mir::AircraftConfig second = SingleEngine();
    second.engines[0].idleRpm = 500.0f;
    second.engines[0].maxRpm = 2500.0f;

    mir::AircraftRuntime runtime = StationaryAir();
    runtime.actuators.throttle = 1.0f;
    for (int i = 0; i < 100; ++i)
        mir::StepPropulsion(first, runtime, kStep);
    REQUIRE(runtime.propulsion.engines[0].rpm > 2000.0f);

    mir::StepPropulsion(second, runtime, 0.0f);
    CHECK(runtime.propulsion.engines[0].rpm == Catch::Approx(500.0f));
}

TEST_CASE("Thrust follows the static curve, the speed factor and the density ratio", "[flight][propulsion]")
{
    const mir::AircraftConfig config = SingleEngine();

    const float staticThrust = ThrustAt(config, glm::vec3{0.0f}, kSeaLevelDensity);
    CHECK(staticThrust == Catch::Approx(4000.0f).margin(1e-3f));
    CHECK(ThrustAt(config, glm::vec3{60.0f, 0.0f, 0.0f}, kSeaLevelDensity)
        == Catch::Approx(0.5f * staticThrust).epsilon(1e-5f));
    CHECK(ThrustAt(config, glm::vec3{200.0f, 0.0f, 0.0f}, kSeaLevelDensity)
        == Catch::Approx(0.3f * staticThrust).epsilon(1e-5f));
    CHECK(ThrustAt(config, glm::vec3{0.0f}, 0.5f * kSeaLevelDensity)
        == Catch::Approx(0.5f * staticThrust).epsilon(1e-5f));
}

TEST_CASE("The engine moment is its thrust offset from the centre of mass", "[flight][propulsion]")
{
    mir::AircraftConfig config = SingleEngine();
    mir::AircraftRuntime runtime = StationaryAir();
    runtime.actuators.throttle = 1.0f;

    mir::StepPropulsion(config, runtime, 0.0f);
    runtime.propulsion.engines[0].rpm = kMaxRpm;
    mir::StepPropulsion(config, runtime, 0.0f);

    const float thrust = runtime.propulsion.force.x;
    CHECK(runtime.propulsion.force == glm::vec3{thrust, 0.0f, 0.0f});
    CHECK(runtime.propulsion.moment == glm::vec3{0.0f});

    config.engines[0].position = glm::vec3{0.0f, 2.0f, 0.0f};
    mir::StepPropulsion(config, runtime, 0.0f);

    CHECK(runtime.propulsion.moment.x == Catch::Approx(0.0f).margin(1e-4f));
    CHECK(runtime.propulsion.moment.y == Catch::Approx(0.0f).margin(1e-4f));
    CHECK(runtime.propulsion.moment.z == Catch::Approx(-2.0f * thrust).margin(1e-3f));
}

TEST_CASE("An aircraft with no engines gets no propulsion force or moment", "[flight][propulsion]")
{
    const mir::AircraftConfig config;
    mir::AircraftRuntime runtime = StationaryAir();
    runtime.airData.relativeVelocityBody = glm::vec3{50.0f, 0.0f, 0.0f};
    runtime.actuators.throttle = 1.0f;

    mir::StepPropulsion(config, runtime, kStep);

    CHECK(runtime.propulsion.force == glm::vec3{0.0f});
    CHECK(runtime.propulsion.moment == glm::vec3{0.0f});
    CHECK(runtime.accumulator.force == glm::vec3{0.0f});
    CHECK(runtime.accumulator.moment == glm::vec3{0.0f});
}

TEST_CASE("Engines parse from the config", "[flight][propulsion][config]")
{
    mir::AircraftConfig config;
    std::string error;

    SECTION("a full engine")
    {
        const std::string engine = EngineJson("[1.5, 0, 0]", "[3, 4, 0]");
        REQUIRE(mir::ParseAircraftConfig(EngineDocument("[" + engine + "]"), config, error));
        REQUIRE(config.engines.size() == 1);
        CHECK(config.engines[0].position == glm::vec3{1.5f, 0.0f, 0.0f});
        CHECK(config.engines[0].direction.x == Catch::Approx(0.6f).margin(1e-5f));
        CHECK(config.engines[0].direction.y == Catch::Approx(0.8f).margin(1e-5f));
        CHECK(config.engines[0].direction.z == Catch::Approx(0.0f).margin(1e-5f));
        CHECK(config.engines[0].idleRpm == Catch::Approx(700.0f));
        CHECK(config.engines[0].maxRpm == Catch::Approx(2700.0f));
        CHECK(config.engines[0].rpmTau == Catch::Approx(0.8f));
        REQUIRE(config.engines[0].staticThrust.size() == 2);
        CHECK(config.engines[0].staticThrust[1].y == Catch::Approx(3200.0f));
        REQUIRE(config.engines[0].speedFactor.size() == 2);
        CHECK(config.engines[0].speedFactor[1].x == Catch::Approx(60.0f));
    }

    SECTION("missing engines")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(BaseDocument() + "}", config, error));
        CHECK(error == "engines: missing");
    }

    SECTION("engines must be an array")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(EngineDocument("3"), config, error));
        CHECK(error == "engines: expected an array");
    }

    SECTION("at most four engines")
    {
        std::string engines = "[";
        for (int i = 0; i < 5; ++i)
        {
            if (i > 0)
                engines += ", ";
            engines += EngineJson();
        }
        engines += "]";

        REQUIRE_FALSE(mir::ParseAircraftConfig(EngineDocument(engines), config, error));
        CHECK(error == "engines: at most 4 engines");
    }

    SECTION("a zero direction is rejected")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(
            EngineDocument("[" + EngineJson("[1.5, 0, 0]", "[0, 0, 0]") + "]"), config, error));
        CHECK(error == "engines[0].direction: must be non-zero");
    }

    SECTION("a negative idle speed is rejected")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(
            EngineDocument("[" + EngineJson("[1.5, 0, 0]", "[1, 0, 0]", "-1") + "]"), config, error));
        CHECK(error == "engines[0].idleRpm: must be >= 0");
    }

    SECTION("maxRpm must be above idleRpm")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(
            EngineDocument("[" + EngineJson("[1.5, 0, 0]", "[1, 0, 0]", "700", "700") + "]"), config, error));
        CHECK(error == "engines[0].maxRpm: must be > idleRpm");
    }

    SECTION("a non-positive rpm time constant is rejected")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(
            EngineDocument("[" + EngineJson("[1.5, 0, 0]", "[1, 0, 0]", "700", "2700", "0") + "]"), config, error));
        CHECK(error == "engines[0].rpmTau: must be > 0");
    }

    SECTION("the static thrust curve needs two points")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(EngineDocument("[" + EngineJson("[1.5, 0, 0]", "[1, 0, 0]", "700",
                                                                   "2700", "0.8", "[[0, 0]]") + "]"),
            config, error));
        CHECK(error == "engines[0].staticThrust: must have at least 2 points");
    }

    SECTION("static thrust x must be strictly increasing")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(EngineDocument("[" + EngineJson("[1.5, 0, 0]", "[1, 0, 0]", "700",
                                                                   "2700", "0.8", "[[0, 0], [0, 1]]") + "]"),
            config, error));
        CHECK(error == "engines[0].staticThrust: x must be strictly increasing");
    }

    SECTION("static thrust x stays within the unit interval")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(EngineDocument("[" + EngineJson("[1.5, 0, 0]", "[1, 0, 0]", "700",
                                                                   "2700", "0.8", "[[0, 0], [1.5, 1]]") + "]"),
            config, error));
        CHECK(error == "engines[0].staticThrust[1][0]: must be in 0..1");
    }

    SECTION("a negative static thrust is rejected")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(EngineDocument("[" + EngineJson("[1.5, 0, 0]", "[1, 0, 0]", "700",
                                                                   "2700", "0.8", "[[0, -1], [1, 1]]") + "]"),
            config, error));
        CHECK(error == "engines[0].staticThrust[0][1]: must be >= 0");
    }

    SECTION("the speed factor curve needs a point")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(EngineDocument("[" + EngineJson("[1.5, 0, 0]", "[1, 0, 0]", "700",
                                                                   "2700", "0.8", "[[0, 0], [1, 3200]]", "[]") + "]"),
            config, error));
        CHECK(error == "engines[0].speedFactor: must not be empty");
    }

    SECTION("speed factor x must be strictly increasing")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(EngineDocument("[" + EngineJson("[1.5, 0, 0]", "[1, 0, 0]", "700",
                                                                   "2700", "0.8", "[[0, 0], [1, 3200]]",
                                                                   "[[0, 1], [0, 0.5]]") + "]"),
            config, error));
        CHECK(error == "engines[0].speedFactor: x must be strictly increasing");
    }

    SECTION("a negative speed factor is rejected")
    {
        REQUIRE_FALSE(mir::ParseAircraftConfig(EngineDocument("[" + EngineJson("[1.5, 0, 0]", "[1, 0, 0]", "700",
                                                                   "2700", "0.8", "[[0, 0], [1, 3200]]",
                                                                   "[[0, -1]]") + "]"),
            config, error));
        CHECK(error == "engines[0].speedFactor[0][1]: must be >= 0");
    }
}

TEST_CASE("A full-throttle engine accelerates the aircraft along its nose", "[flight][propulsion]")
{
    const RawConfig config(kPoweredConfig);
    REQUIRE(config.view.has_value());

    FlightFixture fixture;
    mir::FlightModule flight(fixture.ModuleContext(),
        [&config](std::string_view path, std::string &) -> const mir::AssetBlobView *
        {
            return path == "powered.aircraft.json" ? &*config.view : nullptr;
        });
    fixture.Start();
    const mir::Entity entity = fixture.AddAircraft("powered.aircraft.json");

    mir::Aircraft *aircraft = fixture.world.GetComponent<mir::Aircraft>(entity);
    REQUIRE(aircraft != nullptr);
    aircraft->runtime.actuators.throttle = 1.0f;
    fixture.Run(60);

    const mir::RigidBody *rigidBody = fixture.world.GetComponent<mir::RigidBody>(entity);
    REQUIRE(rigidBody != nullptr);
    const glm::vec3 velocity = fixture.Physics().BodyLinearVelocity(rigidBody->runtime.body);
    CHECK(velocity.z < -0.5f);
    CHECK(std::abs(velocity.x) < 1e-3f);
    CHECK(std::abs(velocity.y) < 1e-3f);

    const mir::Aircraft *updated = fixture.world.GetComponent<mir::Aircraft>(entity);
    REQUIRE(updated != nullptr);
    REQUIRE(updated->runtime.config != nullptr);
    CHECK(updated->runtime.propulsion.force.x > 0.0f);
}
