/**
 * @file LiveTuning.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight/AircraftConfig serialization and the store's live edits.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/AircraftConfig.h>
#include <flight/AircraftConfigStore.h>
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
#include <glm/vec3.hpp>

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
    constexpr std::string_view kPath = "plane.aircraft.json";
    constexpr std::string_view kOtherPath = "other.aircraft.json";

    constexpr std::string_view kFullConfig = R"({
        "version": 1,
        "mass": 1250.0,
        "inertia": { "diagonal": [1100.0, 2200.0, 2700.0], "products": [5.0, 150.0, -20.0] },
        "centerOfMass": [0.5, -0.1, 0.2],
        "reference": { "area": 16.0, "span": 11.0, "chord": 1.5 },
        "camera": { "eye": [0.8, 0.1, -0.4], "chaseDistance": 12.0, "chaseHeight": 3.0 },
        "nodes": { "aileronLeft": "Aileron_L", "elevator": "Elevator" },
        "throttleRate": 0.5,
        "stallWarningMarginDeg": 3.5,
        "actuators": {
            "elevator": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 60.0, "delay": 0.05 },
            "aileron": { "maxDeflectionDeg": 20.0, "rateDegPerSec": 80.0, "delay": 0.04 },
            "rudder": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 60.0, "delay": 0.03 },
            "flaps": { "detentsDeg": [0.0, 10.0, 20.0, 30.0], "rateDegPerSec": 5.0 },
            "trim": { "stepDeg": 0.5, "maxDeg": 10.0 }
        },
        "engines": [
            {
                "position": [0.0, 0.0, -1.5],
                "direction": [0.0, 0.0, -1.0],
                "idleRpm": 800.0,
                "maxRpm": 2700.0,
                "rpmTau": 0.4,
                "staticThrust": [[0.0, 0.0], [0.5, 800.0], [1.0, 1800.0]],
                "speedFactor": [[0.0, 1.0], [60.0, 0.5]]
            }
        ],
        "gear": [
            {
                "name": "nose",
                "mount": [1.2, 0.0, 0.8],
                "restLength": 0.9,
                "stiffness": 40000.0,
                "damping": 2000.0,
                "rollingFriction": 0.02,
                "sideFriction": 0.6,
                "brake": 0.5,
                "maxSteerDeg": 15.0
            },
            {
                "name": "left",
                "mount": [-0.4, 1.1, 0.8],
                "restLength": 0.8,
                "stiffness": 50000.0,
                "damping": 2500.0,
                "rollingFriction": 0.03,
                "sideFriction": 0.7,
                "brake": 0.6,
                "maxSteerDeg": 0.0
            }
        ],
        "aero": {
            "L1": { "CL": 0.4, "CD": 0.04, "Ka": 3.0, "Ke": 4.0, "Kr": 2.0,
                    "Kp": 2000.0, "Kq": 4000.0, "Kn": 2000.0 },
            "L2": {
                "CL0": 0.25, "CLalpha": 5.2, "CD0": 0.03, "k": 0.045,
                "Cm0": 0.02, "Cmalpha": -0.6, "Cmq": -12.0, "Cmde": -1.5,
                "CYbeta": -0.6, "CYdr": 0.2, "Clbeta": -0.12, "Clp": -0.6, "Clda": 0.15,
                "Cnbeta": 0.12, "Cnr": -0.15, "Cndr": -0.08, "Cnda": 0.02,
                "minAirspeed": 12.0,
                "stall": {
                    "positiveDeg": 15.0,
                    "negativeDeg": -12.0,
                    "blendDeg": 6.0,
                    "controlScale": 0.3,
                    "stabilityScale": 0.5,
                    "postStall": [
                        [-12.0, -0.8, 0.2, -0.1],
                        [0.0, 0.25, 0.03, 0.02],
                        [15.0, 1.1, 0.15, -0.2],
                        [21.0, 0.9, 0.25, -0.25]
                    ]
                },
                "flaps": [
                    [0.0, 0.0, 0.0, 0.0, 0.0],
                    [20.0, 0.3, 0.02, -0.05, 2.0]
                ]
            }
        }
    })";

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

    void CheckNear(float actual, float expected)
    {
        CHECK(actual == Catch::Approx(expected).margin(1e-5f));
    }

    void CheckVector(const glm::vec3 &actual, const glm::vec3 &expected)
    {
        CheckNear(actual.x, expected.x);
        CheckNear(actual.y, expected.y);
        CheckNear(actual.z, expected.z);
    }

    void CheckActuator(const mir::AircraftConfig::Actuator &actual, const mir::AircraftConfig::Actuator &expected)
    {
        CheckNear(actual.maxDeflection, expected.maxDeflection);
        CheckNear(actual.rate, expected.rate);
        CheckNear(actual.delay, expected.delay);
    }

    void CheckConfig(const mir::AircraftConfig &actual, const mir::AircraftConfig &expected)
    {
        CHECK(actual.version == expected.version);
        CheckNear(actual.mass, expected.mass);
        CheckVector(actual.inertiaDiagonal, expected.inertiaDiagonal);
        CheckVector(actual.inertiaProducts, expected.inertiaProducts);
        CheckVector(actual.centerOfMass, expected.centerOfMass);
        CheckNear(actual.reference.area, expected.reference.area);
        CheckNear(actual.reference.span, expected.reference.span);
        CheckNear(actual.reference.chord, expected.reference.chord);
        CheckVector(actual.camera.eye, expected.camera.eye);
        CheckNear(actual.camera.chaseDistance, expected.camera.chaseDistance);
        CheckNear(actual.camera.chaseHeight, expected.camera.chaseHeight);
        CheckNear(actual.throttleRate, expected.throttleRate);
        CheckNear(actual.stallWarningMargin, expected.stallWarningMargin);

        REQUIRE(actual.nodes.size() == expected.nodes.size());
        for (std::size_t i = 0; i < actual.nodes.size(); ++i)
        {
            CHECK(actual.nodes[i].component == expected.nodes[i].component);
            CHECK(actual.nodes[i].model == expected.nodes[i].model);
        }

        CheckActuator(actual.actuators.elevator, expected.actuators.elevator);
        CheckActuator(actual.actuators.aileron, expected.actuators.aileron);
        CheckActuator(actual.actuators.rudder, expected.actuators.rudder);
        REQUIRE(actual.actuators.flaps.detents.size() == expected.actuators.flaps.detents.size());
        for (std::size_t i = 0; i < actual.actuators.flaps.detents.size(); ++i)
            CheckNear(actual.actuators.flaps.detents[i], expected.actuators.flaps.detents[i]);
        CheckNear(actual.actuators.flaps.rate, expected.actuators.flaps.rate);
        CheckNear(actual.actuators.trim.step, expected.actuators.trim.step);
        CheckNear(actual.actuators.trim.max, expected.actuators.trim.max);

        REQUIRE(actual.engines.size() == expected.engines.size());
        for (std::size_t i = 0; i < actual.engines.size(); ++i)
        {
            const mir::AircraftConfig::Engine &engine = actual.engines[i];
            const mir::AircraftConfig::Engine &original = expected.engines[i];
            CheckVector(engine.position, original.position);
            CheckVector(engine.direction, original.direction);
            CheckNear(engine.idleRpm, original.idleRpm);
            CheckNear(engine.maxRpm, original.maxRpm);
            CheckNear(engine.rpmTau, original.rpmTau);
            REQUIRE(engine.staticThrust.size() == original.staticThrust.size());
            for (std::size_t j = 0; j < engine.staticThrust.size(); ++j)
            {
                CheckNear(engine.staticThrust[j].x, original.staticThrust[j].x);
                CheckNear(engine.staticThrust[j].y, original.staticThrust[j].y);
            }
            REQUIRE(engine.speedFactor.size() == original.speedFactor.size());
            for (std::size_t j = 0; j < engine.speedFactor.size(); ++j)
            {
                CheckNear(engine.speedFactor[j].x, original.speedFactor[j].x);
                CheckNear(engine.speedFactor[j].y, original.speedFactor[j].y);
            }
        }

        REQUIRE(actual.gear.size() == expected.gear.size());
        for (std::size_t i = 0; i < actual.gear.size(); ++i)
        {
            const mir::AircraftConfig::Gear &point = actual.gear[i];
            const mir::AircraftConfig::Gear &original = expected.gear[i];
            CHECK(point.name == original.name);
            CheckVector(point.mount, original.mount);
            CheckNear(point.restLength, original.restLength);
            CheckNear(point.stiffness, original.stiffness);
            CheckNear(point.damping, original.damping);
            CheckNear(point.rollingFriction, original.rollingFriction);
            CheckNear(point.sideFriction, original.sideFriction);
            CheckNear(point.brake, original.brake);
            CheckNear(point.maxSteer, original.maxSteer);
        }

        REQUIRE(actual.aero.l1.has_value() == expected.aero.l1.has_value());
        if (actual.aero.l1.has_value() && expected.aero.l1.has_value())
        {
            const mir::L1Profile &l1 = *actual.aero.l1;
            const mir::L1Profile &original = *expected.aero.l1;
            CheckNear(l1.CL, original.CL);
            CheckNear(l1.CD, original.CD);
            CheckNear(l1.Ka, original.Ka);
            CheckNear(l1.Ke, original.Ke);
            CheckNear(l1.Kr, original.Kr);
            CheckNear(l1.Kp, original.Kp);
            CheckNear(l1.Kq, original.Kq);
            CheckNear(l1.Kn, original.Kn);
        }

        REQUIRE(actual.aero.l2.has_value() == expected.aero.l2.has_value());
        if (actual.aero.l2.has_value() && expected.aero.l2.has_value())
        {
            const mir::L2Profile &l2 = *actual.aero.l2;
            const mir::L2Profile &original = *expected.aero.l2;
            CheckNear(l2.CL0, original.CL0);
            CheckNear(l2.CLalpha, original.CLalpha);
            CheckNear(l2.CD0, original.CD0);
            CheckNear(l2.k, original.k);
            CheckNear(l2.Cm0, original.Cm0);
            CheckNear(l2.Cmalpha, original.Cmalpha);
            CheckNear(l2.Cmq, original.Cmq);
            CheckNear(l2.Cmde, original.Cmde);
            CheckNear(l2.CYbeta, original.CYbeta);
            CheckNear(l2.CYdr, original.CYdr);
            CheckNear(l2.Clbeta, original.Clbeta);
            CheckNear(l2.Clp, original.Clp);
            CheckNear(l2.Clda, original.Clda);
            CheckNear(l2.Cnbeta, original.Cnbeta);
            CheckNear(l2.Cnr, original.Cnr);
            CheckNear(l2.Cndr, original.Cndr);
            CheckNear(l2.Cnda, original.Cnda);
            CheckNear(l2.minAirspeed, original.minAirspeed);
            CheckNear(l2.stall.positive, original.stall.positive);
            CheckNear(l2.stall.negative, original.stall.negative);
            CheckNear(l2.stall.blend, original.stall.blend);
            CheckNear(l2.stall.controlScale, original.stall.controlScale);
            CheckNear(l2.stall.stabilityScale, original.stall.stabilityScale);
            REQUIRE(l2.stall.postStall.size() == original.stall.postStall.size());
            for (std::size_t i = 0; i < l2.stall.postStall.size(); ++i)
            {
                CheckNear(l2.stall.postStall[i].alpha, original.stall.postStall[i].alpha);
                CheckNear(l2.stall.postStall[i].CL, original.stall.postStall[i].CL);
                CheckNear(l2.stall.postStall[i].CD, original.stall.postStall[i].CD);
                CheckNear(l2.stall.postStall[i].Cm, original.stall.postStall[i].Cm);
            }
            REQUIRE(l2.flaps.size() == original.flaps.size());
            for (std::size_t i = 0; i < l2.flaps.size(); ++i)
            {
                CheckNear(l2.flaps[i].flap, original.flaps[i].flap);
                CheckNear(l2.flaps[i].dCL, original.flaps[i].dCL);
                CheckNear(l2.flaps[i].dCD, original.flaps[i].dCD);
                CheckNear(l2.flaps[i].dCm, original.flaps[i].dCm);
                CheckNear(l2.flaps[i].stallShift, original.flaps[i].stallShift);
            }
        }
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

TEST_CASE("The serializer round-trips a full config", "[flight][config]")
{
    mir::AircraftConfig original;
    std::string error;
    REQUIRE(mir::ParseAircraftConfig(kFullConfig, original, error));

    const std::string serialized = mir::SerializeAircraftConfig(original);
    CHECK(serialized.find("\n  \"version\": 1") != std::string::npos);
    CHECK(serialized.find("\"stallWarningMarginDeg\"") != std::string::npos);
    CHECK(serialized.find("\"detentsDeg\"") != std::string::npos);
    CHECK(serialized.find("\"postStall\"") != std::string::npos);
    CHECK(serialized.find("0000000") == std::string::npos);

    mir::AircraftConfig parsed;
    error.clear();
    REQUIRE(mir::ParseAircraftConfig(serialized, parsed, error));
    CheckConfig(parsed, original);
}

TEST_CASE("A config without tables serializes without them", "[flight][config]")
{
    mir::AircraftConfig original;
    std::string error;
    REQUIRE(mir::ParseAircraftConfig(R"({
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
        "engines": [], "gear": []
    })",
        original, error));

    const std::string serialized = mir::SerializeAircraftConfig(original);
    CHECK(serialized.find("\"aero\"") == std::string::npos);
    CHECK(serialized.find("\"nodes\"") == std::string::npos);

    mir::AircraftConfig parsed;
    error.clear();
    REQUIRE(mir::ParseAircraftConfig(serialized, parsed, error));
    CheckConfig(parsed, original);
}

TEST_CASE("An edited mass re-applies to the body after one fixed step", "[flight][config][tuning]")
{
    const RawConfig plane(kFullConfig);
    REQUIRE(plane.view.has_value());

    auto source = [&plane](std::string_view path, std::string &error) -> const mir::AssetBlobView *
    {
        if (path == kPath)
            return &*plane.view;
        error = "no asset has the path '" + std::string(path) + "'";
        return nullptr;
    };

    FlightFixture fixture;
    mir::FlightModule flight(fixture.ModuleContext(), source);
    fixture.Start();

    const mir::Entity entity = fixture.AddAircraft(kPath);
    fixture.Run(1);

    const mir::RigidBody *rigidBody = fixture.world.GetComponent<mir::RigidBody>(entity);
    REQUIRE(rigidBody != nullptr);
    CHECK(rigidBody->mass == Catch::Approx(1250.0f));

    mir::AircraftConfigStore *store = fixture.world.Resource<mir::AircraftConfigStoreRef>().store;
    REQUIRE(store != nullptr);
    mir::AircraftConfig *edited = store->Edit(kPath);
    REQUIRE(edited != nullptr);
    CHECK(edited == fixture.world.GetComponent<mir::Aircraft>(entity)->runtime.config);

    edited->mass = 2000.0f;
    store->MarkEdited(kPath);
    CHECK(store->IsDirty(kPath));

    fixture.Run(1);
    rigidBody = fixture.world.GetComponent<mir::RigidBody>(entity);
    REQUIRE(rigidBody != nullptr);
    CHECK(rigidBody->mass == Catch::Approx(2000.0f));
}

TEST_CASE("MarkEdited marks the path unsaved and bumps the generation", "[flight][config][tuning]")
{
    const RawConfig plane(kFullConfig);
    REQUIRE(plane.view.has_value());
    mir::AircraftConfigStore store(
        [&plane](std::string_view, std::string &) -> const mir::AssetBlobView * { return &*plane.view; });

    CHECK(store.Get(kPath) != nullptr);
    CHECK(store.Edit("missing.aircraft.json") == nullptr);

    mir::AircraftConfig *edited = store.Edit(kPath);
    REQUIRE(edited != nullptr);
    CHECK(store.EditGeneration() == 0);
    CHECK_FALSE(store.IsDirty(kPath));

    edited->mass = 1500.0f;
    store.MarkEdited(kPath);
    CHECK(store.IsDirty(kPath));
    CHECK(store.EditGeneration() == 1);
    CHECK(store.Generation() == 0);
    CHECK(store.Edit(kPath)->mass == Catch::Approx(1500.0f));

    store.MarkEdited(kPath);
    CHECK(store.EditGeneration() == 2);

    store.ClearDirty(kPath);
    CHECK_FALSE(store.IsDirty(kPath));
    CHECK(store.Edit(kPath)->mass == Catch::Approx(1500.0f));
}

TEST_CASE("A commit over an unsaved path clears it and reports the overwrite", "[flight][config][tuning]")
{
    RawConfig plane(kFullConfig);
    RawConfig other(kFullConfig);
    REQUIRE(plane.view.has_value());
    REQUIRE(other.view.has_value());

    auto source = [&plane, &other](std::string_view path, std::string &error) -> const mir::AssetBlobView *
    {
        if (path == kPath)
            return &*plane.view;
        if (path == kOtherPath)
            return &*other.view;
        error = "no asset has the path '" + std::string(path) + "'";
        return nullptr;
    };

    mir::AircraftConfigStore store(source);
    REQUIRE(store.Get(kPath) != nullptr);
    REQUIRE(store.Get(kOtherPath) != nullptr);

    store.Edit(kPath)->mass = 1500.0f;
    store.MarkEdited(kPath);

    plane = RawConfig(R"({
        "version": 1, "mass": 1900, "inertia": { "diagonal": [1, 2, 3] },
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
        "engines": [], "gear": []
    })");
    REQUIRE(plane.view.has_value());

    store.PrepareReload(source);
    store.CommitReload();

    CHECK_FALSE(store.IsDirty(kPath));
    CHECK(store.Edit(kPath)->mass == Catch::Approx(1900.0f));
    CHECK(store.TakeOverwrittenEdits() == std::vector<std::string>{ std::string(kPath) });
    CHECK(store.TakeOverwrittenEdits().empty());

    store.PrepareReload(source);
    store.CommitReload();
    CHECK(store.TakeOverwrittenEdits().empty());
}
