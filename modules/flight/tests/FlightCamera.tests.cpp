/**
 * @file FlightCamera.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight/FlightCameraSystem.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/AircraftConfig.h>
#include <flight/AircraftRuntime.h>
#include <flight/ComponentRegistration.h>
#include <flight/FlightCameraSystem.h>
#include <flight/FlightFrames.h>
#include <flight/FlightInput.h>
#include <flight/components/Aircraft.h>

#include <camera/ComponentRegistration.h>
#include <camera/components/CameraController.h>
#include <core/ecs/CommandBuffer.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>
#include <input/InputState.h>
#include <renderer/CameraCut.h>
#include <renderer/ComponentRegistration.h>
#include <renderer/components/Camera.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <string>

namespace
{
    constexpr float kStep = 1.0f / 60.0f;

    bool SameVector(const mir::WorldVec3 &actual, const glm::vec3 &expected)
    {
        return glm::length(actual - mir::WorldVec3(expected)) < 1e-5;
    }

    struct CameraFixture
    {
        mir::World world;
        mir::CommandBuffer commands;
        mir::SystemScheduler scheduler;
        mir::SystemContext context{world, commands, kStep, 0.0, 0};
        mir::InputState &input = world.EmplaceResource<mir::InputState>();
        mir::AircraftConfig config;

        mir::Entity aircraft = mir::kNullEntity;
        mir::Entity chase = mir::kNullEntity;
        mir::Entity cockpit = mir::kNullEntity;

        explicit CameraFixture(mir::EngineMode mode = mir::EngineMode::Play, bool withChase = true,
            bool withCockpit = true)
        {
            mir::RegisterCameraComponents();
            mir::RegisterRendererComponents();
            mir::RegisterFlightComponents();
            world.EmplaceResource<mir::EngineModeState>();
            mir::SetEngineMode(world, mode);
            mir::SetPlayState(world, mir::PlayState::Running);

            config.camera.eye = glm::vec3{0.8f, 0.0f, -0.4f};
            config.camera.chaseDistance = 12.0f;
            config.camera.chaseHeight = 3.0f;

            aircraft = world.CreateEntity();
            mir::AddTransform(world, aircraft, mir::Transform{mir::WorldVec3(0.0)});
            mir::Aircraft plane;
            world.AddComponent<mir::Aircraft>(aircraft, plane).runtime.config = &config;

            if (withChase)
            {
                chase = world.CreateEntity();
                mir::AddTransform(world, chase, mir::Transform{mir::WorldVec3(0.0)});
                world.AddComponent<mir::Camera>(chase, mir::Camera{.mActive = true});
                mir::CameraController controller;
                controller.mTarget = aircraft;
                controller.mOffset = glm::vec3{0.0f, 5.0f, 20.0f};
                world.AddComponent<mir::CameraController>(chase, controller);
            }

            if (withCockpit)
            {
                cockpit = world.CreateEntity();
                mir::AddTransform(world, cockpit, mir::Transform{mir::WorldVec3(0.0)}, aircraft);
                world.AddComponent<mir::Camera>(cockpit, mir::Camera{});
            }

            scheduler.AddSystem<mir::FlightCameraSystem>(mir::SystemPhase::Update,
                mir::SystemOrder{mir::SystemOrder::kNormal});
            scheduler.Start(context);
        }

        ~CameraFixture()
        {
            if (scheduler.Started())
                scheduler.Stop(context);
        }

        void Update() { scheduler.Update(context, 1, kStep); }

        // RenderSystem clears the list once it has drawn the frame.
        void Press()
        {
            mir::ClearCameraCuts(world);
            mir::ActionValue &toggle = input.Actions()[std::string(mir::kFlightCameraToggleAction)];
            toggle.justPressed = true;
            Update();
            toggle.justPressed = false;
        }

        mir::Aircraft &Aircraft() { return *world.GetComponent<mir::Aircraft>(aircraft); }
        mir::CameraController &ChaseController() { return *world.GetComponent<mir::CameraController>(chase); }
        mir::Camera &ChaseCamera() { return *world.GetComponent<mir::Camera>(chase); }
        mir::Camera &CockpitCamera() { return *world.GetComponent<mir::Camera>(cockpit); }
        mir::Transform &CockpitTransform() { return *world.GetComponent<mir::Transform>(cockpit); }
    };
}

TEST_CASE("Camera setup wires the chase and cockpit cameras", "[flight][camera]")
{
    CameraFixture fixture;
    fixture.Update();

    const mir::CameraController &controller = fixture.ChaseController();
    CHECK(SameVector(mir::WorldVec3(controller.mOffset), glm::vec3{0.0f, 3.0f, 12.0f}));
    CHECK(controller.mOffsetInTargetSpace);
    CHECK(controller.mOffsetHeadingOnly);
    CHECK(controller.mLookAtTarget);
    CHECK(fixture.ChaseCamera().mActive);

    CHECK(SameVector(fixture.CockpitTransform().Position(), mir::BodyToLocal(fixture.config.camera.eye)));
    CHECK(std::abs(glm::dot(fixture.CockpitTransform().Rotation(), mir::kIdentityRotation)) ==
        Catch::Approx(1.0f));
    CHECK_FALSE(fixture.CockpitCamera().mActive);

    CHECK(fixture.Aircraft().runtime.cockpitCamera == fixture.cockpit);
    CHECK(fixture.Aircraft().runtime.cameraView == mir::FlightCameraView::Chase);
}

TEST_CASE("Camera setup re-runs when the config changes", "[flight][camera]")
{
    CameraFixture fixture;
    fixture.Update();

    mir::AircraftConfig other = fixture.config;
    other.camera.eye = glm::vec3{-0.5f, 0.2f, 0.1f};
    other.camera.chaseDistance = 20.0f;
    other.camera.chaseHeight = 4.0f;
    fixture.Aircraft().runtime.config = &other;

    fixture.Update();

    CHECK(SameVector(mir::WorldVec3(fixture.ChaseController().mOffset), glm::vec3{0.0f, 4.0f, 20.0f}));
    CHECK(SameVector(fixture.CockpitTransform().Position(), mir::BodyToLocal(other.camera.eye)));
    CHECK(fixture.Aircraft().runtime.cameraConfig == &other);
}

TEST_CASE("The camera toggle switches to the cockpit and back with a cut", "[flight][camera]")
{
    CameraFixture fixture;
    fixture.Update();

    fixture.Press();

    CHECK(fixture.CockpitCamera().mActive);
    CHECK_FALSE(fixture.ChaseCamera().mActive);
    CHECK(mir::IsCameraCut(fixture.world, fixture.cockpit));
    CHECK_FALSE(mir::IsCameraCut(fixture.world, fixture.chase));
    CHECK(fixture.Aircraft().runtime.cameraView == mir::FlightCameraView::Cockpit);

    fixture.Press();

    CHECK_FALSE(fixture.CockpitCamera().mActive);
    CHECK(fixture.ChaseCamera().mActive);
    CHECK(mir::IsCameraCut(fixture.world, fixture.chase));
    CHECK_FALSE(mir::IsCameraCut(fixture.world, fixture.cockpit));
    CHECK(fixture.Aircraft().runtime.cameraView == mir::FlightCameraView::Chase);
}

TEST_CASE("Edit mode leaves the cameras as authored", "[flight][camera]")
{
    CameraFixture fixture(mir::EngineMode::Edit);
    fixture.Update();

    const mir::CameraController &controller = fixture.ChaseController();
    CHECK(SameVector(mir::WorldVec3(controller.mOffset), glm::vec3{0.0f, 5.0f, 20.0f}));
    CHECK_FALSE(controller.mOffsetInTargetSpace);
    CHECK_FALSE(controller.mOffsetHeadingOnly);
    CHECK_FALSE(fixture.CockpitCamera().mActive);
    CHECK(SameVector(fixture.CockpitTransform().Position(), glm::vec3{0.0f}));
    CHECK(fixture.Aircraft().runtime.cameraConfig == nullptr);
}

TEST_CASE("The toggle does nothing without both cameras", "[flight][camera]")
{
    SECTION("an aircraft without a cockpit child")
    {
        CameraFixture fixture(mir::EngineMode::Play, true, false);
        fixture.Update();

        fixture.Press();

        CHECK(fixture.ChaseCamera().mActive);
        CHECK_FALSE(mir::IsCameraCut(fixture.world, fixture.chase));
        CHECK(fixture.Aircraft().runtime.cameraView == mir::FlightCameraView::Chase);
    }

    SECTION("an aircraft without a chase camera")
    {
        CameraFixture fixture(mir::EngineMode::Play, false, true);
        fixture.Update();

        fixture.Press();

        CHECK_FALSE(fixture.CockpitCamera().mActive);
        CHECK_FALSE(mir::IsCameraCut(fixture.world, fixture.cockpit));
        CHECK(fixture.Aircraft().runtime.cameraView == mir::FlightCameraView::Chase);
    }
}
