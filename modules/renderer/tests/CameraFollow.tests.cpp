/**
 * @file CameraFollow.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for renderer/CameraFollowSystem: the follow step and the system's
 *        frame ordering.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/CameraFollowSystem.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/WorldTransform.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace
{
    using mir::CameraFollow;
    using mir::FollowPose;
    using mir::WorldVec3;

    double Distance(const WorldVec3 &a, const WorldVec3 &b) { return glm::length(a - b); }

    // An identity camera looks down -Z.
    glm::vec3 Forward(const glm::quat &rotation) { return rotation * glm::vec3(0.0f, 0.0f, -1.0f); }

    CameraFollow MakeFollow()
    {
        CameraFollow follow{};
        follow.mOffset = glm::vec3(0.0f, 2.0f, 6.0f);
        follow.mStiffness = 5.0f;
        follow.mSnapDistance = 50.0f;
        return follow;
    }
}

TEST_CASE("A snap places the camera on its goal, facing the target", "[renderer][camerafollow]")
{
    const CameraFollow follow = MakeFollow();
    const WorldVec3 target(10.0, 0.0, -4.0);
    const WorldVec3 goal = mir::CameraFollowGoal(follow, target, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    REQUIRE(Distance(goal, WorldVec3(10.0, 2.0, 2.0)) < 1e-6);

    const FollowPose next = mir::StepCameraFollow(follow, FollowPose{}, goal, target, 0.016f, true);

    REQUIRE(Distance(next.position, goal) < 1e-6);
    const glm::vec3 toTarget = glm::normalize(glm::vec3(target - next.position));
    REQUIRE(glm::dot(Forward(next.rotation), toTarget) > 0.9999f);
}

TEST_CASE("The follow closes the same share of the gap at any frame rate", "[renderer][camerafollow]")
{
    const CameraFollow follow = MakeFollow();
    const WorldVec3 goal(0.0, 0.0, 10.0);

    const FollowPose oneFrame = mir::StepCameraFollow(follow, FollowPose{}, goal, WorldVec3(0.0), 0.1f, false);

    FollowPose tenFrames{};
    for (int i = 0; i < 10; ++i)
        tenFrames = mir::StepCameraFollow(follow, tenFrames, goal, WorldVec3(0.0), 0.01f, false);

    // 1 - e^(-k t) of the way there, whatever the frame split.
    const double expected = 10.0 * (1.0 - std::exp(-5.0 * 0.1));
    REQUIRE(std::abs(oneFrame.position.z - expected) < 1e-4);
    REQUIRE(std::abs(tenFrames.position.z - expected) < 1e-4);

    // Smooth: it moved, but did not arrive.
    REQUIRE(oneFrame.position.z > 0.0);
    REQUIRE(oneFrame.position.z < 10.0);
}

TEST_CASE("A gap past the snap distance is jumped, not chased", "[renderer][camerafollow]")
{
    CameraFollow follow = MakeFollow();
    follow.mSnapDistance = 20.0f;
    const WorldVec3 goal(0.0, 0.0, 30.0);

    const FollowPose next = mir::StepCameraFollow(follow, FollowPose{}, goal, WorldVec3(0.0), 0.016f, false);
    REQUIRE(Distance(next.position, goal) < 1e-6);

    // 0 turns snapping off.
    follow.mSnapDistance = 0.0f;
    const FollowPose chased = mir::StepCameraFollow(follow, FollowPose{}, goal, WorldVec3(0.0), 0.016f, false);
    REQUIRE(Distance(chased.position, goal) > 1.0);
}

TEST_CASE("Zero stiffness holds the camera still", "[renderer][camerafollow]")
{
    CameraFollow follow = MakeFollow();
    follow.mStiffness = 0.0f;
    follow.mLookAtTarget = false;
    const FollowPose start{ WorldVec3(1.0, 2.0, 3.0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f) };

    const FollowPose next = mir::StepCameraFollow(follow, start, WorldVec3(5.0, 2.0, 3.0), WorldVec3(0.0), 1.0f, false);
    REQUIRE(next.position == start.position);
    REQUIRE(next.rotation == start.rotation);
}

TEST_CASE("A target-space offset turns with the target", "[renderer][camerafollow]")
{
    CameraFollow follow = MakeFollow();
    follow.mOffset = glm::vec3(0.0f, 0.0f, 5.0f);
    const glm::quat quarterTurn = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    follow.mOffsetInTargetSpace = true;
    REQUIRE(Distance(mir::CameraFollowGoal(follow, WorldVec3(0.0), quarterTurn), WorldVec3(5.0, 0.0, 0.0)) < 1e-5);

    follow.mOffsetInTargetSpace = false;
    REQUIRE(Distance(mir::CameraFollowGoal(follow, WorldVec3(0.0), quarterTurn), WorldVec3(0.0, 0.0, 5.0)) < 1e-5);
}

TEST_CASE("Looking straight down keeps the last rotation", "[renderer][camerafollow]")
{
    const CameraFollow follow = MakeFollow();
    const FollowPose start{ WorldVec3(0.0, 10.0, 0.0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f) };

    const FollowPose next = mir::StepCameraFollow(follow, start, start.position, WorldVec3(0.0), 0.016f, true);
    REQUIRE(next.rotation == start.rotation);
}

namespace
{
    // Propagation then follow, registered as App::Initialize does.
    struct FollowFixture
    {
        mir::World world;
        mir::CommandBuffer commands;
        mir::SystemScheduler scheduler;
        mir::SystemContext context{ world, commands, 0.016f, 0.0, 0 };

        mir::Entity target;
        mir::Entity camera;

        explicit FollowFixture(mir::EngineMode mode = mir::EngineMode::Play)
        {
            world.EmplaceResource<mir::EngineModeState>();
            mir::SetEngineMode(world, mode);
            mir::SetPlayState(world, mir::PlayState::Running);

            target = world.CreateEntity();
            mir::AddTransform(world, target, mir::Transform{ WorldVec3(0.0) });

            camera = world.CreateEntity();
            mir::AddTransform(world, camera, mir::Transform{ WorldVec3(0.0, 0.0, 100.0) });
            CameraFollow follow = MakeFollow();
            follow.mTarget = target;
            world.AddComponent<CameraFollow>(camera, follow);

            scheduler.AddSystem<mir::TransformPropagateSystem>(mir::SystemPhase::PostUpdate);
            scheduler.AddSystem<mir::CameraFollowSystem>(mir::SystemPhase::PostUpdate);
            scheduler.Start(context);
        }

        ~FollowFixture()
        {
            if (scheduler.Started())
                scheduler.Stop(context);
        }

        // The cache RenderSystem reads, with no resolve of its own.
        WorldVec3 DrawnCamera() { return world.GetComponent<mir::WorldTransform>(camera)->Position().Meters(); }

        void MoveTarget(const WorldVec3 &position) { world.GetComponent<mir::Transform>(target)->SetPosition(position); }
    };
}

TEST_CASE("The first simulated frame places the camera, and RenderSystem sees it that frame",
    "[renderer][camerafollow]")
{
    FollowFixture fixture;
    fixture.scheduler.Update(fixture.context);

    REQUIRE(Distance(fixture.DrawnCamera(), WorldVec3(0.0, 2.0, 6.0)) < 1e-5);
}

TEST_CASE("The camera chases this frame's target pose", "[renderer][camerafollow]")
{
    FollowFixture fixture;
    fixture.scheduler.Update(fixture.context);

    // Moved before PostUpdate, as a script or physics would.
    fixture.MoveTarget(WorldVec3(4.0, 0.0, 0.0));
    fixture.scheduler.Update(fixture.context);

    const WorldVec3 drawn = fixture.DrawnCamera();
    REQUIRE(drawn.x > 0.0); // already heading for this frame's target
    REQUIRE(drawn.x < 4.0); // without snapping to it
}

TEST_CASE("Edit mode leaves the camera where it was authored", "[renderer][camerafollow]")
{
    FollowFixture fixture(mir::EngineMode::Edit);
    fixture.scheduler.Update(fixture.context);

    REQUIRE(fixture.DrawnCamera() == WorldVec3(0.0, 0.0, 100.0));
}

TEST_CASE("A destroyed target leaves the camera in place", "[renderer][camerafollow]")
{
    FollowFixture fixture;
    fixture.scheduler.Update(fixture.context);
    const WorldVec3 placed = fixture.DrawnCamera();

    fixture.world.DestroyEntity(fixture.target);
    fixture.scheduler.Update(fixture.context);

    REQUIRE(fixture.DrawnCamera() == placed);
}
