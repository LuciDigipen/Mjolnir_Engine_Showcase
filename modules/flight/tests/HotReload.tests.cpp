/**
 * @file HotReload.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight/AircraftConfigStore hot reload and the FlightModule reload stages.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/AircraftConfig.h>
#include <flight/AircraftConfigStore.h>
#include <flight/AircraftRuntime.h>
#include <flight/FlightFrames.h>
#include <flight/FlightModule.h>
#include <flight/components/Aircraft.h>

#include <assets/AssetBlob.h>
#include <camera/ComponentRegistration.h>
#include <camera/components/CameraController.h>
#include <core/ecs/CommandBuffer.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>
#include <physics/ComponentRegistration.h>
#include <physics/PhysicsSystem.h>
#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>
#include <renderer/ComponentRegistration.h>
#include <renderer/components/Camera.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr float kStep = 1.0f / 60.0f;
    constexpr std::string_view kPath = "plane.aircraft.json";

    constexpr std::string_view kLightConfig = R"({
        "version": 1,
        "mass": 1200.0,
        "inertia": { "diagonal": [1000.0, 2000.0, 2500.0], "products": [0.0, 150.0, 0.0] },
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
        },
        "engines": [],
        "gear": []
    })";

    constexpr std::string_view kHeavyConfig = R"({
        "version": 1,
        "mass": 2000.0,
        "inertia": { "diagonal": [3000.0, 4000.0, 5000.0] },
        "centerOfMass": [1.0, 0.0, 0.0],
        "reference": { "area": 20.0, "span": 12.0, "chord": 2.0 },
        "camera": { "eye": [1.0, 0.0, 0.0], "chaseDistance": 20.0, "chaseHeight": 4.0 },
        "throttleRate": 0.5,
        "actuators": {
            "elevator": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 60.0, "delay": 0.05 },
            "aileron": { "maxDeflectionDeg": 20.0, "rateDegPerSec": 80.0, "delay": 0.05 },
            "rudder": { "maxDeflectionDeg": 25.0, "rateDegPerSec": 60.0, "delay": 0.05 },
            "flaps": { "detentsDeg": [0.0, 10.0, 20.0, 30.0], "rateDegPerSec": 5.0 },
            "trim": { "stepDeg": 0.5, "maxDeg": 10.0 }
        },
        "engines": [],
        "gear": []
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

    struct BlobFiles
    {
        std::unordered_map<std::string, RawConfig> files;

        void Set(std::string_view path, std::string_view json)
        {
            files.insert_or_assign(std::string(path), RawConfig(json));
        }
    };

    mir::AircraftConfigStore::Source MakeSource(BlobFiles &files)
    {
        return [&files](std::string_view path, std::string &error) -> const mir::AssetBlobView *
        {
            const auto found = files.files.find(std::string(path));
            if (found == files.files.end() || !found->second.view.has_value())
            {
                error = "no asset has the path '" + std::string(path) + "'";
                return nullptr;
            }
            return &*found->second.view;
        };
    }

    std::function<mir::AircraftConfigStore::Source()> MakeFactory(BlobFiles &files)
    {
        return [&files] { return MakeSource(files); };
    }

    void CheckVector(const glm::vec3 &actual, const glm::vec3 &expected)
    {
        CHECK(actual.x == Catch::Approx(expected.x).margin(1e-4f));
        CHECK(actual.y == Catch::Approx(expected.y).margin(1e-4f));
        CHECK(actual.z == Catch::Approx(expected.z).margin(1e-4f));
    }

    bool SameVector(const mir::WorldVec3 &actual, const glm::vec3 &expected)
    {
        return glm::length(actual - mir::WorldVec3(expected)) < 1e-5;
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
            mir::RegisterCameraComponents();
            mir::RegisterRendererComponents();
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

        void RunStage(mir::Stage stage) { scheduler.RunStage(stage, context); }

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

        mir::Entity AddChaseCamera(mir::Entity aircraft)
        {
            const mir::Entity entity = world.CreateEntity();
            mir::AddTransform(world, entity, mir::Transform{mir::WorldVec3{0.0}});
            world.AddComponent<mir::Camera>(entity, mir::Camera{.mActive = true});
            mir::CameraController controller;
            controller.mTarget = aircraft;
            controller.mOffset = glm::vec3{0.0f, 5.0f, 20.0f};
            world.AddComponent<mir::CameraController>(entity, controller);
            return entity;
        }

        mir::Entity AddCockpitCamera(mir::Entity aircraft)
        {
            const mir::Entity entity = world.CreateEntity();
            mir::AddTransform(world, entity, mir::Transform{mir::WorldVec3{0.0}}, aircraft);
            world.AddComponent<mir::Camera>(entity, mir::Camera{});
            return entity;
        }
    };
}

TEST_CASE("A reload swaps the config in place and re-applies the mass properties", "[flight][reload]")
{
    BlobFiles files;
    files.Set(kPath, kLightConfig);

    FlightFixture fixture;
    mir::FlightModule flight(fixture.ModuleContext(), MakeSource(files), MakeFactory(files));
    fixture.Start();

    const mir::Entity entity = fixture.AddAircraft(kPath);
    fixture.Run(1);

    mir::Aircraft *aircraft = fixture.world.GetComponent<mir::Aircraft>(entity);
    REQUIRE(aircraft != nullptr);
    REQUIRE(aircraft->runtime.config != nullptr);
    const mir::AircraftConfig *const before = aircraft->runtime.config;
    const uint32_t generation = aircraft->runtime.configGeneration;
    CHECK(before->mass == Catch::Approx(1200.0f));

    files.Set(kPath, kHeavyConfig);
    fixture.RunStage(mir::Stage::ReloadPrepare);

    REQUIRE(aircraft->runtime.config == before);
    CHECK(aircraft->runtime.config->mass == Catch::Approx(1200.0f));

    fixture.RunStage(mir::Stage::ReloadCommit);

    REQUIRE(aircraft->runtime.config == before);
    CHECK(aircraft->runtime.config->mass == Catch::Approx(2000.0f));

    const mir::RigidBody *rigidBody = fixture.world.GetComponent<mir::RigidBody>(entity);
    REQUIRE(rigidBody != nullptr);
    CHECK(rigidBody->mass == Catch::Approx(1200.0f));

    fixture.Run(1);

    aircraft = fixture.world.GetComponent<mir::Aircraft>(entity);
    REQUIRE(aircraft != nullptr);
    CHECK(aircraft->runtime.configGeneration > generation);
    rigidBody = fixture.world.GetComponent<mir::RigidBody>(entity);
    REQUIRE(rigidBody != nullptr);
    CHECK(rigidBody->mass == Catch::Approx(2000.0f));
    CheckVector(rigidBody->inertiaDiagonal, glm::vec3{4000.0f, 5000.0f, 3000.0f});
    CheckVector(rigidBody->centerOfMassOffset, glm::vec3{0.0f, 0.0f, -1.0f});
}

TEST_CASE("An invalid candidate vetoes the reload and leaves the old values", "[flight][reload]")
{
    BlobFiles files;
    files.Set(kPath, kLightConfig);

    FlightFixture fixture;
    mir::FlightModule flight(fixture.ModuleContext(), MakeSource(files), MakeFactory(files));
    fixture.Start();

    const mir::Entity entity = fixture.AddAircraft(kPath);
    fixture.Run(1);

    mir::Aircraft *aircraft = fixture.world.GetComponent<mir::Aircraft>(entity);
    REQUIRE(aircraft != nullptr);
    REQUIRE(aircraft->runtime.config != nullptr);
    const mir::AircraftConfig *const before = aircraft->runtime.config;

    files.Set(kPath, "{");
    CHECK_THROWS_AS(fixture.RunStage(mir::Stage::ReloadPrepare), std::runtime_error);
    fixture.RunStage(mir::Stage::ReloadAbort);
    fixture.Run(1);

    aircraft = fixture.world.GetComponent<mir::Aircraft>(entity);
    REQUIRE(aircraft != nullptr);
    REQUIRE(aircraft->runtime.config == before);
    CHECK(aircraft->runtime.config->mass == Catch::Approx(1200.0f));

    const mir::RigidBody *rigidBody = fixture.world.GetComponent<mir::RigidBody>(entity);
    REQUIRE(rigidBody != nullptr);
    CHECK(rigidBody->mass == Catch::Approx(1200.0f));
    CheckVector(rigidBody->inertiaDiagonal, glm::vec3{2000.0f, 2500.0f, 1000.0f});
    CheckVector(rigidBody->inertiaProducts, glm::vec3{0.0f, 0.0f, 150.0f});
}

TEST_CASE("A failed path that becomes valid loads after commit", "[flight][reload]")
{
    BlobFiles files;

    FlightFixture fixture;
    mir::FlightModule flight(fixture.ModuleContext(), MakeSource(files), MakeFactory(files));
    fixture.Start();

    const mir::Entity entity = fixture.AddAircraft(kPath);
    fixture.Run(1);

    mir::Aircraft *aircraft = fixture.world.GetComponent<mir::Aircraft>(entity);
    REQUIRE(aircraft != nullptr);
    CHECK(aircraft->runtime.config == nullptr);
    const mir::RigidBody *untouched = fixture.world.GetComponent<mir::RigidBody>(entity);
    REQUIRE(untouched != nullptr);
    CHECK(untouched->mass == Catch::Approx(1.0f));

    files.Set(kPath, kLightConfig);
    fixture.RunStage(mir::Stage::ReloadPrepare);
    fixture.RunStage(mir::Stage::ReloadCommit);
    fixture.Run(1);

    aircraft = fixture.world.GetComponent<mir::Aircraft>(entity);
    REQUIRE(aircraft != nullptr);
    REQUIRE(aircraft->runtime.config != nullptr);
    CHECK(aircraft->runtime.config->mass == Catch::Approx(1200.0f));

    const mir::RigidBody *rigidBody = fixture.world.GetComponent<mir::RigidBody>(entity);
    REQUIRE(rigidBody != nullptr);
    CHECK(rigidBody->mass == Catch::Approx(1200.0f));
    CheckVector(rigidBody->inertiaDiagonal, glm::vec3{2000.0f, 2500.0f, 1000.0f});
}

TEST_CASE("The store generation changes only on a commit", "[flight][reload]")
{
    BlobFiles files;
    files.Set(kPath, kLightConfig);

    mir::AircraftConfigStore store(MakeSource(files));
    const mir::AircraftConfig *const before = store.Get(kPath);
    REQUIRE(before != nullptr);
    CHECK(store.Generation() == 0);

    store.PrepareReload(MakeSource(files));
    CHECK(store.Generation() == 0);
    store.AbortReload();
    CHECK(store.Generation() == 0);

    files.Set(kPath, kHeavyConfig);
    store.PrepareReload(MakeSource(files));
    store.CommitReload();
    CHECK(store.Generation() == 1);

    const mir::AircraftConfig *const after = store.Get(kPath);
    CHECK(after == before);
    CHECK(after->mass == Catch::Approx(2000.0f));

    files.Set(kPath, "{");
    CHECK_THROWS_AS(store.PrepareReload(MakeSource(files)), std::runtime_error);
    store.AbortReload();
    CHECK(store.Generation() == 1);

    files.Set(kPath, kLightConfig);
    store.PrepareReload(MakeSource(files));
    store.CommitReload();
    CHECK(store.Generation() == 2);
    const mir::AircraftConfig *const reloaded = store.Get(kPath);
    REQUIRE(reloaded != nullptr);
    CHECK(reloaded->mass == Catch::Approx(1200.0f));
}

TEST_CASE("Only a module with a reload factory registers the reload stages", "[flight][reload]")
{
    BlobFiles files;
    files.Set(kPath, kLightConfig);

    SECTION("without a factory")
    {
        FlightFixture fixture;
        mir::FlightModule flight(fixture.ModuleContext(), MakeSource(files));
        CHECK(fixture.scheduler.FunctionCount(mir::Stage::ReloadPrepare) == 0);
        CHECK(fixture.scheduler.FunctionCount(mir::Stage::ReloadCommit) == 0);
        CHECK(fixture.scheduler.FunctionCount(mir::Stage::ReloadAbort) == 0);
    }

    SECTION("with a factory")
    {
        FlightFixture fixture;
        mir::FlightModule flight(fixture.ModuleContext(), MakeSource(files), MakeFactory(files));
        CHECK(fixture.scheduler.FunctionCount(mir::Stage::ReloadPrepare) == 1);
        CHECK(fixture.scheduler.FunctionCount(mir::Stage::ReloadCommit) == 1);
        CHECK(fixture.scheduler.FunctionCount(mir::Stage::ReloadAbort) == 1);
    }
}

TEST_CASE("A reload re-runs the camera setup", "[flight][reload][camera]")
{
    BlobFiles files;
    files.Set(kPath, kLightConfig);

    FlightFixture fixture;
    mir::FlightModule flight(fixture.ModuleContext(), MakeSource(files), MakeFactory(files));
    fixture.Start();

    const mir::Entity entity = fixture.AddAircraft(kPath);
    const mir::Entity chase = fixture.AddChaseCamera(entity);
    const mir::Entity cockpit = fixture.AddCockpitCamera(entity);
    fixture.Run(2);

    mir::Aircraft *aircraft = fixture.world.GetComponent<mir::Aircraft>(entity);
    REQUIRE(aircraft != nullptr);
    REQUIRE(aircraft->runtime.config != nullptr);
    mir::CameraController *controller = fixture.world.GetComponent<mir::CameraController>(chase);
    REQUIRE(controller != nullptr);
    CHECK(SameVector(mir::WorldVec3(controller->mOffset), glm::vec3{0.0f, 3.0f, 12.0f}));

    files.Set(kPath, kHeavyConfig);
    fixture.RunStage(mir::Stage::ReloadPrepare);
    fixture.RunStage(mir::Stage::ReloadCommit);
    fixture.Run(2);

    aircraft = fixture.world.GetComponent<mir::Aircraft>(entity);
    REQUIRE(aircraft != nullptr);
    const mir::AircraftConfig *const config = aircraft->runtime.config;
    REQUIRE(config != nullptr);
    CHECK(config->mass == Catch::Approx(2000.0f));
    CHECK(aircraft->runtime.cameraConfig == config);
    controller = fixture.world.GetComponent<mir::CameraController>(chase);
    REQUIRE(controller != nullptr);
    CHECK(SameVector(mir::WorldVec3(controller->mOffset), glm::vec3{0.0f, 4.0f, 20.0f}));

    const mir::Transform *cockpitTransform = fixture.world.GetComponent<mir::Transform>(cockpit);
    REQUIRE(cockpitTransform != nullptr);
    CHECK(SameVector(cockpitTransform->Position(), mir::WorldVec3(mir::BodyToLocal(config->camera.eye))));
}
