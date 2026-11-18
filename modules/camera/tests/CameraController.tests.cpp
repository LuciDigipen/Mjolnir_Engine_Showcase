/**
 * @file CameraController.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for camera/CameraControllerSystem: the follow and free-fly steps,
 *        switching between them, and the system's frame ordering.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <camera/CameraControllerSystem.h>
#include <camera/components/CameraController.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/WorldTransform.h>
#include <input/DefaultInputMap.h>
#include <input/InputState.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <string>
#include <string_view>

namespace
{
    using mir::CameraController;
    using mir::CameraMode;
    using mir::CameraPose;
    using mir::FreeFlyInput;
    using mir::WorldVec3;

    double Distance(const WorldVec3 &a, const WorldVec3 &b) { return glm::length(a - b); }

    // An identity camera looks down -Z.
    glm::vec3 Forward(const glm::quat &rotation) { return rotation * glm::vec3(0.0f, 0.0f, -1.0f); }

    CameraController MakeFollow()
    {
        CameraController controller{};
        controller.mOffset = glm::vec3(0.0f, 2.0f, 6.0f);
        controller.mStiffness = 5.0f;
        controller.mSnapDistance = 50.0f;
        return controller;
    }
}

TEST_CASE("A snap places the camera on its goal, facing the target", "[camera][follow]")
{
    const CameraController follow = MakeFollow();
    const WorldVec3 target(10.0, 0.0, -4.0);
    const WorldVec3 goal = mir::CameraFollowGoal(follow, target, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    REQUIRE(Distance(goal, WorldVec3(10.0, 2.0, 2.0)) < 1e-6);

    const CameraPose next = mir::StepCameraFollow(follow, CameraPose{}, goal, target, 0.016f, true);

    REQUIRE(Distance(next.position, goal) < 1e-6);
    const glm::vec3 toTarget = glm::normalize(glm::vec3(target - next.position));
    REQUIRE(glm::dot(Forward(next.rotation), toTarget) > 0.9999f);
}

TEST_CASE("The follow closes the same share of the gap at any frame rate", "[camera][follow]")
{
    const CameraController follow = MakeFollow();
    const WorldVec3 goal(0.0, 0.0, 10.0);

    const CameraPose oneFrame = mir::StepCameraFollow(follow, CameraPose{}, goal, WorldVec3(0.0), 0.1f, false);

    CameraPose tenFrames{};
    for (int i = 0; i < 10; ++i)
        tenFrames = mir::StepCameraFollow(follow, tenFrames, goal, WorldVec3(0.0), 0.01f, false);

    // 1 - e^(-k t) of the way there, whatever the frame split.
    const double expected = 10.0 * (1.0 - std::exp(-5.0 * 0.1));
    REQUIRE(std::abs(oneFrame.position.z - expected) < 1e-4);
    REQUIRE(std::abs(tenFrames.position.z - expected) < 1e-4);

    REQUIRE(oneFrame.position.z > 0.0);
    REQUIRE(oneFrame.position.z < 10.0);
}

TEST_CASE("A gap past the snap distance is jumped, not chased", "[camera][follow]")
{
    CameraController follow = MakeFollow();
    follow.mSnapDistance = 20.0f;
    const WorldVec3 goal(0.0, 0.0, 30.0);

    const CameraPose next = mir::StepCameraFollow(follow, CameraPose{}, goal, WorldVec3(0.0), 0.016f, false);
    REQUIRE(Distance(next.position, goal) < 1e-6);

    // 0 turns snapping off.
    follow.mSnapDistance = 0.0f;
    const CameraPose chased = mir::StepCameraFollow(follow, CameraPose{}, goal, WorldVec3(0.0), 0.016f, false);
    REQUIRE(Distance(chased.position, goal) > 1.0);
}

TEST_CASE("Returning from free-fly chases a far goal instead of jumping to it", "[camera][follow]")
{
    CameraController follow = MakeFollow();
    follow.mSnapDistance = 20.0f;
    follow.mReturning = true;
    const WorldVec3 goal(0.0, 0.0, 30.0);

    const CameraPose next = mir::StepCameraFollow(follow, CameraPose{}, goal, WorldVec3(0.0), 0.016f, false);
    REQUIRE(Distance(next.position, goal) > 1.0);
}

TEST_CASE("Zero stiffness holds the camera still", "[camera][follow]")
{
    CameraController follow = MakeFollow();
    follow.mStiffness = 0.0f;
    follow.mLookAtTarget = false;
    const CameraPose start{ WorldVec3(1.0, 2.0, 3.0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f) };

    const CameraPose next = mir::StepCameraFollow(follow, start, WorldVec3(5.0, 2.0, 3.0), WorldVec3(0.0), 1.0f, false);
    REQUIRE(next.position == start.position);
    REQUIRE(next.rotation == start.rotation);
}

TEST_CASE("A target-space offset turns with the target", "[camera][follow]")
{
    CameraController follow = MakeFollow();
    follow.mOffset = glm::vec3(0.0f, 0.0f, 5.0f);
    const glm::quat quarterTurn = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    follow.mOffsetInTargetSpace = true;
    REQUIRE(Distance(mir::CameraFollowGoal(follow, WorldVec3(0.0), quarterTurn), WorldVec3(5.0, 0.0, 0.0)) < 1e-5);

    follow.mOffsetInTargetSpace = false;
    REQUIRE(Distance(mir::CameraFollowGoal(follow, WorldVec3(0.0), quarterTurn), WorldVec3(0.0, 0.0, 5.0)) < 1e-5);
}

TEST_CASE("A heading-only offset follows the target's yaw", "[camera][follow]")
{
    CameraController follow = MakeFollow();
    follow.mOffset = glm::vec3(0.0f, 3.0f, 8.0f);
    follow.mOffsetInTargetSpace = true;
    follow.mOffsetHeadingOnly = true;

    const glm::quat yaw = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const WorldVec3 target(2.0, -1.5, 4.0);
    const WorldVec3 goal = mir::CameraFollowGoal(follow, target, yaw);

    REQUIRE(std::abs(goal.x - (target.x + 8.0)) < 1e-4);
    REQUIRE(std::abs(goal.y - (target.y + 3.0)) < 1e-4);
    REQUIRE(std::abs(goal.z - target.z) < 1e-4);
}

TEST_CASE("A heading-only offset ignores the target's roll and pitch", "[camera][follow]")
{
    CameraController follow = MakeFollow();
    follow.mOffset = glm::vec3(0.0f, 3.0f, 8.0f);
    follow.mOffsetInTargetSpace = true;
    follow.mOffsetHeadingOnly = true;

    const glm::quat yaw = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const WorldVec3 target(2.0, -1.5, 4.0);
    const WorldVec3 level = mir::CameraFollowGoal(follow, target, yaw);

    const glm::quat rolled = yaw * glm::angleAxis(glm::radians(60.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::quat pitched = yaw * glm::angleAxis(glm::radians(30.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::quat both = pitched * glm::angleAxis(glm::radians(60.0f), glm::vec3(0.0f, 0.0f, 1.0f));

    for (const glm::quat &rotation : { rolled, pitched, both })
    {
        const WorldVec3 goal = mir::CameraFollowGoal(follow, target, rotation);
        REQUIRE(Distance(goal, level) < 1e-4);
        REQUIRE(goal.y - target.y == 3.0);
    }
}

TEST_CASE("A heading-only offset stays finite with the target pointed straight up", "[camera][follow]")
{
    CameraController follow = MakeFollow();
    follow.mOffset = glm::vec3(0.0f, 3.0f, 8.0f);
    follow.mOffsetInTargetSpace = true;
    follow.mOffsetHeadingOnly = true;

    const glm::quat straightUp = glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::quat yawedStraightUp = glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f)) * straightUp;

    for (const glm::quat &rotation : { straightUp, yawedStraightUp })
    {
        const WorldVec3 goal = mir::CameraFollowGoal(follow, WorldVec3(0.0), rotation);
        REQUIRE(std::isfinite(goal.x));
        REQUIRE(std::isfinite(goal.y));
        REQUIRE(std::isfinite(goal.z));
    }
}

TEST_CASE("Without heading only, a target-space offset turns with the target's roll", "[camera][follow]")
{
    CameraController follow = MakeFollow();
    follow.mOffset = glm::vec3(0.0f, 3.0f, 8.0f);
    follow.mOffsetInTargetSpace = true;
    follow.mOffsetHeadingOnly = false;

    const glm::quat roll = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    REQUIRE(Distance(mir::CameraFollowGoal(follow, WorldVec3(0.0), roll), WorldVec3(-3.0, 0.0, 8.0)) < 1e-4);
}

TEST_CASE("Looking straight down keeps the last rotation", "[camera][follow]")
{
    const CameraController follow = MakeFollow();
    const CameraPose start{ WorldVec3(0.0, 10.0, 0.0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f) };

    const CameraPose next = mir::StepCameraFollow(follow, start, start.position, WorldVec3(0.0), 0.016f, true);
    REQUIRE(next.rotation == start.rotation);
}

TEST_CASE("Free-fly seeded from a rotation keeps facing the same way", "[camera][freefly]")
{
    CameraController controller{};
    const glm::vec3 facing = glm::normalize(glm::vec3(0.3f, -0.4f, -1.0f));
    mir::SeedFreeFly(controller, glm::quatLookAt(facing, glm::vec3(0.0f, 1.0f, 0.0f)));

    const WorldVec3 start(4.0, 5.0, 6.0);
    const CameraPose next = mir::StepFreeFly(controller, start, FreeFlyInput{}, 0.016f);

    REQUIRE(next.position == start);
    REQUIRE(glm::dot(Forward(next.rotation), facing) > 0.9999f);
}

TEST_CASE("Free-fly moves along its view at moveSpeed, and faster held fast", "[camera][freefly]")
{
    CameraController controller{}; // yaw -90, pitch 0: facing -Z
    controller.mMoveSpeed = 5.0f;
    controller.mFastMultiplier = 4.0f;

    FreeFlyInput input;
    input.move = { 0.0f, -1.0f }; // forward
    CameraPose next = mir::StepFreeFly(controller, WorldVec3(0.0), input, 0.5f);
    REQUIRE(Distance(next.position, WorldVec3(0.0, 0.0, -2.5)) < 1e-5);

    input.fast = true;
    next = mir::StepFreeFly(controller, WorldVec3(0.0), input, 0.5f);
    REQUIRE(Distance(next.position, WorldVec3(0.0, 0.0, -10.0)) < 1e-5);

    // Strafe right and rise.
    input = FreeFlyInput{};
    input.move = { 1.0f, 0.0f };
    input.vertical = 1.0f;
    next = mir::StepFreeFly(controller, WorldVec3(0.0), input, 1.0f);
    REQUIRE(Distance(next.position, WorldVec3(5.0, 5.0, 0.0)) < 1e-5);
}

TEST_CASE("Free-fly turns by pixels from the mouse and by a rate from the stick", "[camera][freefly]")
{
    CameraController controller{};
    controller.mLookSensitivity = 0.1f;
    controller.mStickLookRate = 90.0f;

    FreeFlyInput input;
    input.lookPixels = { 100.0f, 0.0f };
    mir::StepFreeFly(controller, WorldVec3(0.0), input, 0.016f);
    REQUIRE(std::abs(controller.mYawDegrees - (-90.0f + 10.0f)) < 1e-4f);

    input = FreeFlyInput{};
    input.lookStick = { 0.0f, -1.0f }; // stick up looks up
    mir::StepFreeFly(controller, WorldVec3(0.0), input, 0.5f);
    REQUIRE(std::abs(controller.mPitchDegrees - 45.0f) < 1e-4f);

    // Pitch stops short of straight up.
    input = FreeFlyInput{};
    input.lookPixels = { 0.0f, -100000.0f };
    mir::StepFreeFly(controller, WorldVec3(0.0), input, 0.016f);
    REQUIRE(controller.mPitchDegrees == 89.0f);
}

TEST_CASE("Free-fly faces a yaw and pitch written through the field table", "[camera][freefly]")
{
    const auto findField = [](std::string_view name) -> const mir::FieldDesc *
    {
        for (const mir::FieldDesc &field : mir::kCameraControllerFields)
            if (field.mName == name)
                return &field;
        return nullptr;
    };
    const mir::FieldDesc *yaw = findField("yawDegrees");
    const mir::FieldDesc *pitch = findField("pitchDegrees");
    REQUIRE(yaw != nullptr);
    REQUIRE(pitch != nullptr);
    CHECK(yaw->mHidden);
    CHECK(pitch->mHidden);

    CameraController controller{};
    float value = 0.0f; // +X
    REQUIRE(yaw->Write(&controller, &value));
    value = 30.0f;
    REQUIRE(pitch->Write(&controller, &value));

    const CameraPose next = mir::StepFreeFly(controller, WorldVec3(0.0), FreeFlyInput{}, 0.016f);
    const glm::vec3 expected(std::cos(glm::radians(30.0f)), std::sin(glm::radians(30.0f)), 0.0f);
    REQUIRE(glm::dot(Forward(next.rotation), expected) > 0.9999f);

    value = 120.0f;
    REQUIRE(pitch->Constrain(&value));
    REQUIRE(value == mir::kFreeFlyMaxPitchDegrees);
}

namespace
{
    // Propagation then the controller, registered as App::Initialize does.
    struct ControllerFixture
    {
        mir::World world;
        mir::CommandBuffer commands;
        mir::SystemScheduler scheduler;
        mir::SystemContext context{ world, commands, 0.016f, 0.0, 0 };
        mir::InputState &input = world.EmplaceResource<mir::InputState>();

        mir::Entity target;
        mir::Entity camera;

        explicit ControllerFixture(mir::EngineMode mode = mir::EngineMode::Play)
        {
            world.EmplaceResource<mir::EngineModeState>();
            mir::SetEngineMode(world, mode);
            mir::SetPlayState(world, mir::PlayState::Running);

            target = world.CreateEntity();
            mir::AddTransform(world, target, mir::Transform{ WorldVec3(0.0) });

            camera = world.CreateEntity();
            mir::AddTransform(world, camera, mir::Transform{ WorldVec3(0.0, 0.0, 100.0) });
            CameraController controller = MakeFollow();
            controller.mTarget = target;
            world.AddComponent<CameraController>(camera, controller);

            scheduler.AddSystem<mir::TransformPropagateSystem>(mir::SystemPhase::Propagate, mir::SystemOrder{mir::SystemOrder::kNormal});
            scheduler.AddSystem<mir::CameraControllerSystem>(mir::SystemPhase::LateUpdate, mir::SystemOrder{mir::SystemOrder::kNormal});
            scheduler.Start(context);
        }

        ~ControllerFixture()
        {
            if (scheduler.Started())
                scheduler.Stop(context);
        }

        void Update() { scheduler.Update(context); }

        // What InputSystem would have written this frame.
        mir::ActionValue &Action(std::string_view name) { return input.Actions()[std::string(name)]; }

        void PressToggle()
        {
            Action(mir::kToggleCameraModeAction).justPressed = true;
            Update();
            Action(mir::kToggleCameraModeAction).justPressed = false;
        }

        CameraController &Controller() { return *world.GetComponent<CameraController>(camera); }
        mir::Transform &CameraTransform() { return *world.GetComponent<mir::Transform>(camera); }

        // The cache RenderSystem reads, with no resolve of its own.
        WorldVec3 DrawnCamera() { return world.GetComponent<mir::WorldTransform>(camera)->Position().Meters(); }

        void MoveTarget(const WorldVec3 &position) { world.GetComponent<mir::Transform>(target)->SetPosition(position); }
    };
}

TEST_CASE("The first simulated frame places the camera, and RenderSystem sees it that frame", "[camera][follow]")
{
    ControllerFixture fixture;
    fixture.Update();

    REQUIRE(Distance(fixture.DrawnCamera(), WorldVec3(0.0, 2.0, 6.0)) < 1e-5);
}

TEST_CASE("The camera chases this frame's target pose", "[camera][follow]")
{
    ControllerFixture fixture;
    fixture.Update();

    // Moved before PostUpdate, as a script or physics would.
    fixture.MoveTarget(WorldVec3(4.0, 0.0, 0.0));
    fixture.Update();

    const WorldVec3 drawn = fixture.DrawnCamera();
    REQUIRE(drawn.x > 0.0);
    REQUIRE(drawn.x < 4.0);
}

TEST_CASE("Edit mode leaves the camera where it was authored", "[camera]")
{
    ControllerFixture fixture(mir::EngineMode::Edit);
    fixture.Controller().mMode = CameraMode::FreeFly;
    fixture.Action(mir::kCameraMoveAction).y = -1.0f;
    fixture.Update();

    REQUIRE(fixture.DrawnCamera() == WorldVec3(0.0, 0.0, 100.0));
}

TEST_CASE("A destroyed target leaves the camera in place", "[camera][follow]")
{
    ControllerFixture fixture;
    fixture.Update();
    const WorldVec3 placed = fixture.DrawnCamera();

    fixture.world.DestroyEntity(fixture.target);
    fixture.Update();

    REQUIRE(fixture.DrawnCamera() == placed);
}

TEST_CASE("Toggling to free-fly keeps the camera's position and view", "[camera][freefly]")
{
    ControllerFixture fixture;
    fixture.Update();
    const WorldVec3 placed = fixture.DrawnCamera();
    const glm::vec3 facing = Forward(fixture.CameraTransform().Rotation());

    fixture.PressToggle();

    REQUIRE(fixture.Controller().mMode == CameraMode::FreeFly);
    REQUIRE(Distance(fixture.DrawnCamera(), placed) < 1e-6);
    REQUIRE(glm::dot(Forward(fixture.CameraTransform().Rotation()), facing) > 0.9999f);
}

TEST_CASE("Toggling back to follow chases from where free-fly left the camera", "[camera][freefly]")
{
    ControllerFixture fixture;
    fixture.Update();
    fixture.PressToggle();

    // Fly far past the snap distance.
    fixture.CameraTransform().SetPosition(WorldVec3(0.0, 0.0, 300.0));
    fixture.Update();

    fixture.PressToggle();
    REQUIRE(fixture.Controller().mMode == CameraMode::Follow);

    const WorldVec3 drawn = fixture.DrawnCamera();
    REQUIRE(drawn.z < 300.0);
    REQUIRE(drawn.z > 100.0); // heading back, not snapped
}

TEST_CASE("Free-fly flies while paused, and follow waits", "[camera][freefly]")
{
    ControllerFixture fixture;
    fixture.Update();
    const WorldVec3 placed = fixture.DrawnCamera();

    mir::SetPlayState(fixture.world, mir::PlayState::Paused);
    fixture.MoveTarget(WorldVec3(10.0, 0.0, 0.0));
    fixture.Update();
    REQUIRE(fixture.DrawnCamera() == placed);

    fixture.Controller().mMode = CameraMode::FreeFly;
    fixture.Action(mir::kCameraMoveAction).y = -1.0f;
    fixture.Update();
    REQUIRE(Distance(fixture.DrawnCamera(), placed) > 0.01);
}

TEST_CASE("Free-fly holds still while a game UI holds the input", "[camera][freefly]")
{
    ControllerFixture fixture;
    fixture.Controller().mMode = CameraMode::FreeFly;
    fixture.Update();
    const WorldVec3 start = fixture.DrawnCamera();

    fixture.input.SetGameInputHeld(true);
    fixture.Action(mir::kCameraMoveAction).y = -1.0f;
    fixture.Update();

    REQUIRE(fixture.DrawnCamera() == start);
}

TEST_CASE("Mouse look needs CameraLookHold", "[camera][freefly]")
{
    ControllerFixture fixture;
    fixture.Controller().mMode = CameraMode::FreeFly;
    fixture.Update();
    const float yaw = fixture.Controller().mYawDegrees;

    fixture.Action(mir::kCameraLookAction).x = 50.0f;
    fixture.Update();
    REQUIRE(fixture.Controller().mYawDegrees == yaw);

    fixture.Action(mir::kCameraLookHoldAction).down = true;
    fixture.Update();
    REQUIRE(fixture.Controller().mYawDegrees != yaw);
}
