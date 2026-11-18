/**
 * @file FlightStepGate.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight/FlightStepGate.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/FlightStepGate.h>

#include <flight/AircraftRuntime.h>
#include <flight/FlightFrames.h>
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

#include <glm/gtc/quaternion.hpp>

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
    constexpr float kTolerance = 1e-5f;

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
        },
        "engines": [],
        "gear": []
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

    mir::BodyHandle CreateFreeBody(mir::PhysicsWorld &physics)
    {
        mir::BodyDesc desc;
        desc.mMotion = mir::PhysicsMotion::Dynamic;
        desc.mShape = mir::PhysicsShape::Box;
        desc.mHalfExtent = glm::vec3{1.0f};
        desc.mMass = 1.0f;
        desc.mLinearDamping = 0.0f;
        desc.mAngularDamping = 0.0f;
        desc.mAllowSleeping = false;
        desc.mCustomInertia = true;
        desc.mInertiaDiagonal = glm::vec3{1.0f};
        return physics.CreateBody(desc);
    }

    glm::vec3 WorldTorqueFor(const glm::vec3 &bodyMoment)
    {
        return mir::BodyToLocal(bodyMoment);
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

        mir::PhysicsWorld &Physics() { return world.Resource<mir::PhysicsWorld>(); }

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

TEST_CASE("Control moments turn the aircraft the right way", "[flight][frames]")
{
    SECTION("A positive moment about body +Y raises the nose")
    {
        mir::PhysicsWorld physics{ mir::PhysicsWorldDesc{ .mGravity = glm::vec3{0.0f} } };
        const mir::BodyHandle body = CreateFreeBody(physics);
        const glm::vec3 torque = WorldTorqueFor(glm::vec3{0.0f, 1.0f, 0.0f});

        for (int i = 0; i < 30; ++i)
        {
            physics.AddTorque(body, torque);
            physics.Step(kStep);
        }

        const glm::vec3 nose = physics.BodyRotation(body) * glm::vec3{0.0f, 0.0f, -1.0f};
        CHECK(nose.y > 0.05f);
        CHECK(physics.BodyAngularVelocity(body).x > 0.0f);
    }

    SECTION("A positive moment about body +X drops the right wing")
    {
        mir::PhysicsWorld physics{ mir::PhysicsWorldDesc{ .mGravity = glm::vec3{0.0f} } };
        const mir::BodyHandle body = CreateFreeBody(physics);
        const glm::vec3 torque = WorldTorqueFor(glm::vec3{1.0f, 0.0f, 0.0f});

        for (int i = 0; i < 30; ++i)
        {
            physics.AddTorque(body, torque);
            physics.Step(kStep);
        }

        const glm::vec3 rightWing = physics.BodyRotation(body) * glm::vec3{1.0f, 0.0f, 0.0f};
        CHECK(rightWing.y < -0.05f);
        CHECK(physics.BodyAngularVelocity(body).z < 0.0f);
    }

    SECTION("A positive moment about body +Z turns the nose toward world +X")
    {
        mir::PhysicsWorld physics{ mir::PhysicsWorldDesc{ .mGravity = glm::vec3{0.0f} } };
        const mir::BodyHandle body = CreateFreeBody(physics);
        const glm::vec3 torque = WorldTorqueFor(glm::vec3{0.0f, 0.0f, 1.0f});

        for (int i = 0; i < 30; ++i)
        {
            physics.AddTorque(body, torque);
            physics.Step(kStep);
        }

        const glm::vec3 nose = physics.BodyRotation(body) * glm::vec3{0.0f, 0.0f, -1.0f};
        CHECK(nose.x > 0.05f);
        CHECK(physics.BodyAngularVelocity(body).y < 0.0f);
    }
}

TEST_CASE("The gate fills the aircraft state from the step", "[flight][gate]")
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

    fixture.Run(1);

    const mir::RigidBody *rigidBody = fixture.world.GetComponent<mir::RigidBody>(entity);
    REQUIRE(rigidBody != nullptr);
    const mir::BodyHandle body = rigidBody->runtime.body;
    const std::optional<mir::WorldPosition> position = fixture.Physics().BodyPosition(body);
    REQUIRE(position.has_value());

    const mir::AircraftState &state = fixture.world.GetComponent<mir::Aircraft>(entity)->runtime.state;
    CHECK(state.position.x == Catch::Approx(position->Meters().x).margin(kTolerance));
    CHECK(state.position.y == Catch::Approx(position->Meters().y).margin(kTolerance));
    CHECK(state.position.z == Catch::Approx(position->Meters().z).margin(kTolerance));
    CHECK(state.linearVelocity.x == Catch::Approx(fixture.Physics().BodyLinearVelocity(body).x).margin(kTolerance));
    CHECK(state.linearVelocity.y == Catch::Approx(fixture.Physics().BodyLinearVelocity(body).y).margin(kTolerance));
    CHECK(state.linearVelocity.z == Catch::Approx(fixture.Physics().BodyLinearVelocity(body).z).margin(kTolerance));
    CHECK(state.rotation.w == Catch::Approx(1.0f).margin(kTolerance));

    fixture.Physics().SetBodyAngularVelocity(body, glm::vec3{0.0f, 0.0f, 3.0f});
    fixture.Run(1);

    const mir::AircraftState &turned = fixture.world.GetComponent<mir::Aircraft>(entity)->runtime.state;
    CHECK(turned.angularVelocity.z == Catch::Approx(3.0f).margin(kTolerance));
    CHECK(turned.angularVelocityBody.x == Catch::Approx(-3.0f).margin(kTolerance));
    CHECK(turned.angularVelocityBody.y == Catch::Approx(0.0f).margin(kTolerance));
    CHECK(turned.angularVelocityBody.z == Catch::Approx(0.0f).margin(kTolerance));
}

TEST_CASE("The flight gate detaches when the module goes", "[flight][gate]")
{
    FlightFixture fixture;

    {
        mir::FlightModule flight(fixture.ModuleContext(), {});
        REQUIRE(fixture.world.Resource<mir::PhysicsStepGates>().mGates.size() == 1);
        fixture.Start();
        fixture.AddAircraft();
        fixture.Run(1);
    }

    CHECK(fixture.world.Resource<mir::PhysicsStepGates>().mGates.empty());
    fixture.Run(1);
}
