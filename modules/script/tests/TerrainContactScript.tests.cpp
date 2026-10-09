/**
 * @file TerrainContactScript.tests.cpp
 * @author Jackey Cen
 * @brief Tests for games/TerrainCheck terrain_contact.lua.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <script/ScriptHost.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/fs/FileIO.h>
#include <core/log/Log.h>
#include <core/log/LogHistory.h>
#include <physics/CollisionEventSystem.h>
#include <physics/CollisionEvents.h>
#include <physics/PhysicsSystem.h>
#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>
#include <terrain/ComponentRegistration.h>
#include <terrain/components/TerrainInstance.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace
{
    constexpr float kStep = 1.0f / 60.0f;

    // A ball scripted with the shipped terrain_contact.lua, dropped onto a height field tagged
    // with the ground entity, driven the way App does: scripts, then one substep and the publish.
    struct Fixture
    {
        mir::World world;
        mir::CommandBuffer commands;
        mir::SystemScheduler scheduler;
        mir::SystemContext context{ world, commands, kStep, 0.0, 0 };
        mir::ScriptHost host;
        std::vector<float> samples = std::vector<float>(17 * 17, 0.0f);
        mir::Entity ground;
        mir::Entity ball;
        int32_t instance = -1;

        explicit Fixture(bool groundIsTerrain)
        {
            // The script reacts only through print, which reaches LogHistory once logging is on.
            mir::InitLog({ .filePath = (std::filesystem::temp_directory_path() / "mir_terrain_contact_tests.log").string() });
            mir::LogHistory::Clear();

            mir::RegisterTerrainComponents();
            world.EmplaceResource<mir::PhysicsWorld>();
            world.EmplaceResource<mir::CollisionEvents>();
            world.EmplaceResource<mir::EngineModeState>();
            mir::SetEngineMode(world, mir::EngineMode::Play);
            mir::SetPlayState(world, mir::PlayState::Running);

            scheduler.AddSystem<mir::PhysicsSystem>(mir::SystemPhase::FixedUpdate);
            scheduler.AddSystem<mir::CollisionEventSystem>(mir::SystemPhase::PostUpdate);
            scheduler.Start(context);

            const std::optional<std::string> source =
                mir::ReadText(std::filesystem::path(MIR_GAMES_DIR) / "TerrainCheck/assets/scripts/terrain_contact.lua");
            REQUIRE(source);
            REQUIRE(host.LoadScriptSource("terrain_contact", *source));

            ground = world.CreateEntity();
            if (groundIsTerrain)
                world.AddComponent<mir::TerrainInstance>(ground, mir::TerrainInstance{});
            mir::HeightFieldDesc desc;
            desc.mSamples = samples;
            desc.mCountX = 17;
            desc.mCountZ = 17;
            desc.mSpacing = 1.0f;
            desc.mOrigin = *mir::WorldPosition::FromMeters(mir::WorldVec3(-8.0, 0.0, -8.0));
            const mir::BodyHandle field = Physics().CreateStaticHeightField(desc);
            REQUIRE(field.IsValid());
            Physics().SetBodyEntity(field, ground);

            ball = world.CreateEntity();
            mir::AddTransform(world, ball, mir::Transform{ mir::WorldVec3(0.25, 1.5, 0.25) });
            world.AddComponent<mir::RigidBody>(ball, mir::RigidBody{
                                                         .motion = mir::PhysicsMotion::Dynamic,
                                                         .shape = mir::PhysicsShape::Sphere,
                                                         .radius = 0.5f,
                                                     });
            instance = host.CreateInstance("terrain_contact");
            REQUIRE(instance != -1);
        }

        ~Fixture()
        {
            host.DestroyInstance(instance);
            if (scheduler.Started())
                scheduler.Stop(context);
            mir::ShutdownLog();
        }

        mir::PhysicsWorld &Physics() { return world.Resource<mir::PhysicsWorld>(); }

        void Frames(int count)
        {
            for (int i = 0; i < count; ++i)
            {
                host.CallOnUpdate(world, commands, ball, instance, kStep);
                commands.Flush(world);
                scheduler.Update(context, 1, kStep);
            }
        }

        int Reactions() const
        {
            int count = 0;
            for (const mir::LogEntry &entry : mir::LogHistory::Snapshot())
                if (entry.message.starts_with("[lua] terrain_contact: first terrain contact"))
                    ++count;
            return count;
        }
    };
}

TEST_CASE("terrain_contact.lua logs the first terrain contact once", "[script][physics][collision]")
{
    Fixture fixture(true);

    fixture.Frames(120);

    CHECK(fixture.Reactions() == 1);
}

TEST_CASE("terrain_contact.lua ignores ground without a TerrainInstance", "[script][physics][collision]")
{
    Fixture fixture(false);

    fixture.Frames(120);

    CHECK(fixture.Reactions() == 0);
}
