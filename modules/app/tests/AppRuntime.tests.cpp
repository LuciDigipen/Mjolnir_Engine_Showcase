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
#include <input/InputMap.h>
#include <physics_layers/PhysicsLayers.h>
#include <renderer/PostProcessingSettings.h>
#include <renderer/RenderSettings.h>
#include <renderer/ViewDrawSource.h>
#include <scene/SceneIO.h>
#include <terrain/TerrainStreamingConfiguration.h>

#include <catch2/catch_test_macros.hpp>
#include <glm/vec3.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace
{
    constexpr int kMaterialFrameLimit = 3000;
    constexpr int kSettleFrames = 30;

    struct TestLog
    {
        TestLog()
        {
            mir::InitLog({ .filePath = path.string() });
        }

        ~TestLog()
        {
            mir::ShutdownLog();
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }

        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("mjolnir_app_terrain_material_test_" + std::to_string(mir::CurrentProcessId()) + ".log");
    };

    mir::AppDesc TerrainDesc(bool editor)
    {
        mir::AppDesc desc;
        desc.mProject = mir::ResolveGameProject(
            (std::filesystem::path(MIR_APP_TEST_ROOT) / "games/TerrainCheck").string(), mir::ErrorCategory::Usage);
        mir::ResolveAssetIdRoot(desc.mProject);
        desc.mHidden = true;
        desc.mNonInteractive = true;
        desc.mWorkerThreads = 1;
        desc.mEnableEditorLayout = editor;
        desc.mMaxFrames = kMaterialFrameLimit;
        return desc;
    }

    std::vector<mir::WorldDrawItem> TerrainDraws(const mir::World &world)
    {
        std::vector<mir::WorldDrawItem> draws;
        if (const auto *sources = world.TryResource<mir::ViewDrawSources>())
            for (mir::IViewDrawSource *source : sources->sources)
                source->AppendRayTracingDraws(0, draws);
        return draws;
    }

    mir::MaterialHandle TerrainMaterial(const mir::World &world)
    {
        const std::vector<mir::WorldDrawItem> draws = TerrainDraws(world);
        return draws.empty() ? mir::kNullMaterial : draws.front().material;
    }
}

TEST_CASE("Play and Stop isolate authoring while scene swaps retain the runtime World", "[app][smoke]")
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
    desc.mWorkerThreads = 1;
    desc.mEnableEditorLayout = true;
    desc.mMaxFrames = 240;

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
                const auto first = mir::AllocateSceneInstanceId();
                const auto second = mir::AllocateSceneInstanceId();
                const auto cancelled = mir::AllocateSceneInstanceId();
                const auto finalAdditive = mir::AllocateSceneInstanceId();
                mir::SceneInstanceId base = mir::kOwnedBySession;
                mir::Entity a, b, spawnedA, spawnedB, baseEntity;
                int phase = 0;
                const auto additiveChecks = app.Systems().AddFrameSlot(mir::FrameSlot::FrameBegin,
                    [&](mir::SystemContext &)
                    {
                        auto &world = *app.RuntimeWorld();
                        const auto &scenes = world.Resource<mir::SceneCollection>();
                        auto &requests = world.Resource<mir::RequestQueue>();
                        if (phase == 0 && world.PrimaryScene() != previousScene)
                        {
                            base = world.PrimaryScene();
                            baseEntity = scenes.FindEntity(world, base, 1);
                            requests.PushAdditiveScene({ "newScene", first });
                            requests.PushAdditiveScene({ "newScene", second });
                            requests.PushAdditiveScene({ "newScene", cancelled });
                            requests.PushUnloadScene(cancelled);
                            requests.PushPrimaryScene(second);
                            phase = 1;
                        }
                        else if (phase == 1 && scenes.Instances().size() == 3 && world.PrimaryScene() == second)
                        {
                            CHECK(scenes.Find(cancelled) == nullptr);
                            a = scenes.FindEntity(world, first, 1);
                            b = scenes.FindEntity(world, second, 1);
                            CHECK(a != b);
                            CHECK(world.IsAlive(baseEntity));
                            spawnedB = world.CreateEntity();
                            {
                                const mir::World::SceneScope scope(world, first);
                                spawnedA = world.CreateEntity();
                            }
                            CHECK(mir::SetParent(world, b, a));
                            requests.PushUnloadScene(first);
                            phase = 2;
                        }
                        else if (phase == 2 && scenes.Find(first) == nullptr)
                        {
                            CHECK_FALSE(world.IsAlive(a));
                            CHECK_FALSE(world.IsAlive(spawnedA));
                            CHECK(world.IsAlive(b));
                            CHECK(world.HierarchyIndex().ParentOf(b).IsNull());
                            CHECK(world.IsAlive(spawnedB));
                            CHECK(world.IsAlive(baseEntity));
                            requests.PushUnloadScene(second);
                            phase = 3;
                        }
                        else if (phase == 3 && scenes.Find(second) == nullptr)
                        {
                            CHECK_FALSE(world.IsAlive(b));
                            CHECK_FALSE(world.IsAlive(spawnedB));
                            CHECK(world.PrimaryScene() == base);
                            CHECK(world.IsAlive(persistent));
                            requests.PushUnloadScene(base);
                            phase = 4;
                        }
                        else if (phase == 4 && scenes.Instances().empty())
                        {
                            CHECK(world.PrimaryScene() == mir::kOwnedBySession);
                            CHECK(world.IsAlive(persistent));
                            requests.PushNextScene("newScene");
                            phase = 5;
                        }
                        else if (phase == 5 && scenes.Instances().size() == 1)
                        {
                            requests.PushAdditiveScene({ "demo_beacons", finalAdditive });
                            phase = 6;
                        }
                        else if (phase == 6 && scenes.Instances().size() == 2 && scenes.Find(finalAdditive))
                            phase = 7;
                    });
                runtime->Resource<mir::RequestQueue>().PushNextScene("demo_gallery");
                app.Run();
                CHECK(phase == 7);
                mir::World *destination = app.RuntimeWorld();
                REQUIRE(destination != nullptr);
                CHECK(destination == runtime);
                CHECK(destination->ActiveScene() != previousScene);
                CHECK(destination->IsAlive(persistent));
                CHECK(destination->EntityScene(persistent) == mir::kOwnedBySession);
                CHECK_FALSE(destination->IsAlive(spawned));
                CHECK(destination->Resource<int>() == 42);
                CHECK(destination->TrySceneResource<int>(previousScene) == nullptr);
                size_t sceneEntities = 0;
                destination->ForEachAliveEntity([&](mir::Entity entity)
                    { if (destination->EntityScene(entity) != mir::kOwnedBySession) ++sceneEntities; });
                CHECK(sceneEntities == 3);
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

TEST_CASE("An unknown scene swap keeps a pending additive load", "[app][smoke]")
{
    const std::filesystem::path logPath = std::filesystem::temp_directory_path() /
        ("mjolnir_app_swap_test_" + std::to_string(mir::CurrentProcessId()) + ".log");
    mir::InitLog({ .filePath = logPath.string() });

    mir::GameProject project = mir::ResolveGameProject(
        (std::filesystem::path(MIR_APP_TEST_ROOT) / "games/HelloWorld").string(), mir::ErrorCategory::Usage);
    mir::ResolveAssetIdRoot(project);

    mir::AppDesc desc;
    desc.mProject = project;
    desc.mHidden = true;
    desc.mNonInteractive = true;
    desc.mWorkerThreads = 1;
    desc.mEnableEditorLayout = true;
    desc.mMaxFrames = 120;

    {
        mir::App app(desc);
        app.PlayScene();
        REQUIRE(app.RuntimeWorld() != nullptr);

        const mir::SceneInstanceId beacons = mir::AllocateSceneInstanceId();
        auto &requests = app.RuntimeWorld()->Resource<mir::RequestQueue>();
        requests.PushAdditiveScene({ "demo_beacons", beacons });
        requests.PushNextScene("no_such_scene");
        app.Run();

        REQUIRE(app.RuntimeWorld() != nullptr);
        CHECK(app.RuntimeWorld()->Resource<mir::SceneCollection>().Find(beacons) != nullptr);
        app.StopScene();
    }

    mir::ShutdownLog();
    std::error_code ec;
    std::filesystem::remove(logPath, ec);
}

TEST_CASE("The authoring and Play Worlds share one set of project resources", "[app][smoke]")
{
    const std::filesystem::path logPath = std::filesystem::temp_directory_path() /
        ("mjolnir_app_resources_test_" + std::to_string(mir::CurrentProcessId()) + ".log");
    mir::InitLog({ .filePath = logPath.string() });

    mir::GameProject project = mir::ResolveGameProject(
        (std::filesystem::path(MIR_APP_TEST_ROOT) / "games/HelloWorld").string(), mir::ErrorCategory::Usage);
    mir::ResolveAssetIdRoot(project);

    mir::AppDesc desc;
    desc.mProject = project;
    desc.mHidden = true;
    desc.mNonInteractive = true;
    desc.mWorkerThreads = 1;
    desc.mEnableEditorLayout = true;
    desc.mMaxFrames = 1;

    SECTION("Editor creates Play on request") { desc.mEnableEditorLayout = true; }
    SECTION("Standalone creates Play during startup") { desc.mEnableEditorLayout = false; }

    {
        mir::App app(desc);
        mir::World &authored = app.GetWorld();
        app.PlayScene();
        mir::World *runtime = app.RuntimeWorld();
        REQUIRE(runtime != nullptr);

        runtime->Resource<mir::ControlsSettings>().mInvertMouseY =
            !runtime->Resource<mir::ControlsSettings>().mInvertMouseY;
        const bool inverted = runtime->Resource<mir::ControlsSettings>().mInvertMouseY;
        CHECK(&runtime->Resource<mir::ControlsSettings>() == &authored.Resource<mir::ControlsSettings>());
        CHECK(&runtime->Resource<mir::InputMap>() == &authored.Resource<mir::InputMap>());
        CHECK(&runtime->Resource<mir::RenderSettings>() == &authored.Resource<mir::RenderSettings>());
        CHECK(&runtime->Resource<mir::PostProcessingSettings>() == &authored.Resource<mir::PostProcessingSettings>());
        CHECK(&runtime->Resource<mir::PhysicsLayerTable>() == &authored.Resource<mir::PhysicsLayerTable>());
        CHECK(&runtime->Resource<mir::TerrainStreamingConfiguration>() ==
              &authored.Resource<mir::TerrainStreamingConfiguration>());

        runtime->Resource<mir::RenderSettings>().rayTracing = !runtime->Resource<mir::RenderSettings>().rayTracing;
        const bool changed = runtime->Resource<mir::RenderSettings>().rayTracing;
        app.StopScene();
        CHECK(authored.Resource<mir::RenderSettings>().rayTracing == changed);
        CHECK(authored.Resource<mir::ControlsSettings>().mInvertMouseY == inverted);
        app.PlayScene();
        REQUIRE(app.RuntimeWorld() != nullptr);
        CHECK(app.RuntimeWorld()->Resource<mir::ControlsSettings>().mInvertMouseY == inverted);
        app.StopScene();
    }

    mir::ShutdownLog();
    std::error_code ec;
    std::filesystem::remove(logPath, ec);
}

TEST_CASE("Controls menu edits and saves the shared player preferences", "[app][controls]")
{
    const auto path = std::filesystem::temp_directory_path() /
        ("mjolnir_controls_adapter_" + std::to_string(mir::CurrentProcessId()) + ".json");
    mir::ControlsSettings settings;
    mir::GameControlsOptions options(settings, path);
    options.SetInvertY(mir::LookDevice::Mouse, true);
    CHECK(settings.mInvertMouseY);
    CHECK(options.InvertY(mir::LookDevice::Mouse));
    CHECK(mir::LoadControlsSettings(path).mInvertMouseY);
    options.SetInvertY(mir::LookDevice::Gamepad, true);
    CHECK(settings.mInvertMouseY);
    CHECK(settings.mInvertGamepadY);
    options.RestoreDefaults();
    CHECK(settings == mir::ControlsSettings{});
    CHECK(mir::LoadControlsSettings(path) == settings);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("Stopping Play keeps the Edit terrain's material handle live", "[app][smoke][terrain-material]")
{
    const TestLog log;
    mir::App app(TerrainDesc(true));
    mir::World &authored = app.GetWorld();
    auto &requests = authored.Resource<mir::RequestQueue>();

    constexpr int kCycles = 2;
    mir::MaterialHandle edit = mir::kNullMaterial;
    int stops = 0;
    int settled = 0;
    bool redrawn = false;
    const auto checks = app.Systems().AddFrameSlot(mir::FrameSlot::FrameBegin, [&](mir::SystemContext &)
        {
            if (const mir::World *runtime = app.RuntimeWorld())
            {
                if (TerrainMaterial(*runtime).IsNull() || ++settled < kSettleFrames)
                    return;
                requests.Push(mir::Request::Stop);
                ++stops;
                settled = 0;
                redrawn = false;
                return;
            }
            const mir::MaterialHandle current = TerrainMaterial(authored);
            if (edit.IsNull())
            {
                if (current.IsNull())
                    return;
                edit = current;
                REQUIRE(app.Renderer().MaterialRecord(edit).has_value());
                requests.Push(mir::Request::Play);
                return;
            }
            if (stops == 0)
                return;
            // Play suspends Edit's views, so it has no draws until a render after Stop prepares them again.
            if (!redrawn && TerrainDraws(authored).empty())
                return;
            redrawn = true;
            CHECK(current == edit);
            CHECK(app.Renderer().MaterialRecord(edit).has_value());
            if (++settled < kSettleFrames)
                return;
            settled = 0;
            requests.Push(stops < kCycles ? mir::Request::Play : mir::Request::Quit);
        });
    app.Run();

    CHECK(stops == kCycles);
    CHECK(redrawn);
    CHECK(app.RuntimeWorld() == nullptr);
}

TEST_CASE("A full scene unload resets the library after the terrain lets go", "[app][smoke][terrain-material]")
{
    const TestLog log;
    mir::App app(TerrainDesc(true));

    mir::MaterialHandle edit = mir::kNullMaterial;
    const auto checks = app.Systems().AddFrameSlot(mir::FrameSlot::FrameBegin, [&](mir::SystemContext &)
        {
            if (!edit.IsNull())
                return;
            edit = TerrainMaterial(app.GetWorld());
            if (!edit.IsNull())
                app.GetWorld().Resource<mir::RequestQueue>().Push(mir::Request::Quit);
        });
    app.Run();
    REQUIRE_FALSE(edit.IsNull());
    REQUIRE_FALSE(app.SurfaceMaterials().ActiveMaterials().empty());

    // The renderer destroys handles after their frames finish, but a reset moves the generation at once.
    const uint64_t generation = app.SurfaceMaterials().Generation();
    app.NewScene();
    CHECK(app.SurfaceMaterials().ActiveMaterials().empty());
    CHECK(app.SurfaceMaterials().Generation() > generation);
}

TEST_CASE("A standalone scene swap releases the outgoing terrain's material", "[app][smoke][terrain-material]")
{
    const TestLog log;
    mir::AppDesc desc = TerrainDesc(false);
    desc.mProject.mScenes.push_back(
        { "empty", std::filesystem::path(MIR_APP_TEST_ROOT) / "games/EmptyScene/scenes/default" });
    mir::App app(desc);

    mir::MaterialHandle outgoing = mir::kNullMaterial;
    bool released = false;
    const auto checks = app.Systems().AddFrameSlot(mir::FrameSlot::FrameBegin, [&](mir::SystemContext &)
        {
            mir::World &runtime = *app.RuntimeWorld();
            if (outgoing.IsNull())
            {
                outgoing = TerrainMaterial(runtime);
                if (!outgoing.IsNull())
                    runtime.Resource<mir::RequestQueue>().PushNextScene("empty");
            }
            else if (!released && !app.Renderer().MaterialRecord(outgoing).has_value())
            {
                released = true;
                app.GetWorld().Resource<mir::RequestQueue>().Push(mir::Request::Quit);
            }
        });
    app.Run();

    REQUIRE_FALSE(outgoing.IsNull());
    CHECK(released);
}
