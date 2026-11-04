/**
 * @file AppRuntime.tests.cpp
 * @author sumin.park
 * @brief Verifies Play session isolation and repeated teardown through App.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <app/App.h>

#include <assets/AssetIdRoot.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/Requests.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/components/Transform.h>
#include <core/log/Log.h>
#include <core/platform/Process.h>
#include <scene/SceneIO.h>

#include <catch2/catch_test_macros.hpp>
#include <glm/vec3.hpp>

#include <filesystem>
#include <string>

TEST_CASE("Play and Stop keep the authored scene while replacing the runtime world", "[app][smoke]")
{
    const std::filesystem::path logPath = std::filesystem::temp_directory_path() /
        ("mjolnir_app_runtime_test_" + std::to_string(mir::CurrentProcessId()) + ".log");
    mir::InitLog({ .filePath = logPath.string() });

    mir::GameProject project = mir::ResolveGameProject(
        (std::filesystem::path(MIR_APP_TEST_ROOT) / "games/HelloWorld").string(), mir::ErrorCategory::Usage);
    mir::ResolveAssetIdRoot(project);

    mir::AppDesc desc;
    desc.mProject = project;
    desc.mHidden = true;
    desc.mNonInteractive = true;
    desc.mWorkerThreads = 0;
    desc.mEnableEditorLayout = true;
    desc.mMaxFrames = 1;

    {
        mir::App app(desc);
        mir::World &authored = app.GetWorld();
        const mir::Entity entity = mir::CreateSceneEntity(authored, app.Scene());
        mir::AddTransform(authored, entity, mir::Transform{ glm::vec3(71.0f, 72.0f, 73.0f) });
        app.Scene().mDirty = true;
        REQUIRE(authored.IsAlive(entity));
        const bool wasDirty = app.Scene().mDirty;
        const mir::WorldVec3 authoredPosition = authored.GetComponent<mir::Transform>(entity)->Position();

        for (int cycle = 0; cycle < 3; ++cycle)
        {
            app.PlayScene();
            mir::World *runtime = app.RuntimeWorld();
            REQUIRE(runtime != nullptr);
            CHECK(runtime != &authored);
            CHECK(mir::IsPlaying(*runtime));
            CHECK(mir::IsPlaying(authored));

            bool foundUnsavedEdit = false;
            runtime->ForEachAliveEntity([&](mir::Entity current)
                {
                    mir::Transform *transform = runtime->GetComponent<mir::Transform>(current);
                    if (transform == nullptr)
                        return;
                    if (transform->Position() == authoredPosition)
                        foundUnsavedEdit = true;
                    transform->SetPosition(glm::vec3(99.0f));
                });
            CHECK(foundUnsavedEdit);

            const mir::Entity spawned = runtime->CreateEntity();
            CHECK(runtime->IsAlive(spawned));
            app.TogglePause();
            CHECK(mir::IsPaused(*runtime));
            app.TogglePause();
            CHECK_FALSE(mir::IsPaused(*runtime));

            if (cycle == 0)
            {
                const mir::SceneInstanceId previousScene = runtime->ActiveScene();
                runtime->EmplaceResource<int>(42);
                runtime->EmplaceSceneResource<int>(previousScene, 7);
                const mir::Entity persistent = runtime->CreateEntity();
                REQUIRE(runtime->PersistEntity(persistent));
                runtime->Resource<mir::RequestQueue>().PushNextScene("newScene");
                app.Run();
                mir::World *destination = app.RuntimeWorld();
                REQUIRE(destination != nullptr);
                CHECK(destination == runtime);
                CHECK(destination->ActiveScene() != previousScene);
                CHECK(destination->IsAlive(persistent));
                CHECK(destination->EntityScene(persistent) == mir::kOwnedBySession);
                CHECK_FALSE(destination->IsAlive(spawned));
                CHECK(destination->Resource<int>() == 42);
                CHECK(destination->TrySceneResource<int>(previousScene) == nullptr);
                size_t entities = 0;
                destination->ForEachAliveEntity([&](mir::Entity) { ++entities; });
                CHECK(entities == 2);
            }

            app.StopScene();
            CHECK(app.RuntimeWorld() == nullptr);
            CHECK(mir::IsEditing(authored));
            CHECK(authored.IsAlive(entity));
            CHECK(authored.GetComponent<mir::Transform>(entity)->Position() == authoredPosition);
            CHECK(app.Scene().mDirty == wasDirty);
        }
    }

    mir::ShutdownLog();
    std::error_code ec;
    std::filesystem::remove(logPath, ec);
}
