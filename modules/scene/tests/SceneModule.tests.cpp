/**
 * @file SceneModule.tests.cpp
 * @author Quy Dong Ta
 * @brief An untitled scene does not save over the broken scene it stands in for.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <scene/SceneIO.h>
#include <scene/SceneModule.h>
#include <scene/SceneLoader.h>

#include <jobs/JobExecutor.h>

#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>

namespace
{
    using mir::ModuleContext;
    using mir::SceneModule;
    using mir::SystemScheduler;
    using mir::World;

    struct TempSceneDir
    {
        std::filesystem::path mPath;

        explicit TempSceneDir(std::string_view name)
            : mPath(std::filesystem::temp_directory_path() / "mir_scene_module_tests" / name)
        {
            std::filesystem::remove_all(mPath);
        }

        ~TempSceneDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(mPath, ec);
        }

        TempSceneDir(const TempSceneDir &) = delete;
        TempSceneDir &operator=(const TempSceneDir &) = delete;
    };

    void WriteText(const std::filesystem::path &path, std::string_view text)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file << text;
    }

    std::string ReadText(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    }

    // a corrupt scene.json beside the entity and instance files of the scene it held
    void WriteBrokenScene(const std::filesystem::path &dir)
    {
        WriteText(dir / "scene.json", R"({"name":"broken","entities":[1)");
        WriteText(dir / "entities" / "1.json", R"({"id":1,"parent":0,"components":[]})");
        WriteText(dir / "instances" / "2.json", R"({"prefab":"p","root":2})");
    }

    void CheckBrokenSceneIntact(const std::filesystem::path &dir)
    {
        CHECK(ReadText(dir / "scene.json") == R"({"name":"broken","entities":[1)");
        CHECK(ReadText(dir / "entities" / "1.json") == R"({"id":1,"parent":0,"components":[]})");
        CHECK(ReadText(dir / "instances" / "2.json") == R"({"prefab":"p","root":2})");
    }
}

TEST_CASE("Scene unload keeps session entities and other scene instances", "[scene]")
{
    World world;
    SystemScheduler scheduler;
    SceneModule scene({ world, scheduler }, {});
    const mir::SceneInstanceId outgoing = scene.Scene().mInstanceId;
    const mir::Entity parent = mir::CreateSceneEntity(world, scene.Scene());
    const mir::Entity persistent = mir::CreateSceneEntity(world, scene.Scene(), parent);
    const mir::Entity child = mir::CreateSceneEntity(world, scene.Scene(), persistent);
    REQUIRE(world.PersistEntity(persistent));
    const mir::Entity spawned = world.CreateEntity();
    world.EmplaceResource<int>(23);
    world.EmplaceSceneResource<int>(outgoing, 11);

    const mir::SceneInstanceId destination = mir::AllocateSceneInstanceId();
    world.EmplaceSceneResource<int>(destination, 17);
    mir::Entity prepared;
    {
        const World::SceneScope scope(world, destination);
        prepared = world.CreateEntity();
    }
    scene.Unload();
    CHECK_FALSE(world.IsAlive(parent));
    CHECK_FALSE(world.IsAlive(spawned));
    CHECK(world.IsAlive(persistent));
    CHECK(world.IsAlive(child));
    CHECK(world.IsAlive(prepared));
    CHECK(world.HierarchyIndex().ParentOf(persistent).IsNull());
    CHECK(world.HierarchyIndex().ParentOf(child) == persistent);
    CHECK(world.EntityScene(child) == mir::kOwnedBySession);
    CHECK(world.EntityScene(prepared) == destination);
    CHECK(world.TrySceneResource<int>(outgoing) == nullptr);
    REQUIRE(world.TrySceneResource<int>(destination) != nullptr);
    CHECK(*world.TrySceneResource<int>(destination) == 17);
    CHECK(world.Resource<int>() == 23);
    scene.New("again");
    CHECK(scene.Scene().mInstanceId != outgoing);
    CHECK(scene.LastLoad().instance == scene.Scene().mInstanceId);
}

TEST_CASE("Prepared scene activation uses the captured files", "[scene][prepare]")
{
    TempSceneDir dir("prepared_files");
    WriteText(dir.mPath / "scene.json", R"({"name":"captured","entities":[1],"nextId":2})");
    WriteText(dir.mPath / "entities/1.json", R"({"id":1,"components":[]})");
    const mir::PreparedScene prepared = mir::PrepareScene(dir.mPath);
    REQUIRE(prepared.probe.Ok());
    REQUIRE_FALSE(prepared.cancelled);
    std::filesystem::remove_all(dir.mPath);

    World world;
    const auto previous = mir::AllocateSceneInstanceId();
    world.SetActiveScene(previous);
    const auto loaded = mir::LoadPreparedScene(world, prepared);
    CHECK(loaded.mComplete);
    CHECK(loaded.mName == "captured");
    REQUIRE(loaded.mEntities.size() == 1);
    CHECK(world.EntityScene(loaded.mEntities.at(1)) == loaded.mInstanceId);
    CHECK(world.ActiveScene() == previous);
}

TEST_CASE("A cancelled preparation never materializes entities", "[scene][prepare]")
{
    std::stop_source stop;
    stop.request_stop();
    const auto prepared = mir::PrepareScene({}, stop.get_token());
    CHECK(prepared.cancelled);
    World world;
    const auto loaded = mir::LoadPreparedScene(world, prepared);
    CHECK_FALSE(loaded.mComplete);
    CHECK(loaded.mEntities.empty());
}

TEST_CASE("Scene loader discards cancelled and stale ready results", "[scene][prepare]")
{
    TempSceneDir dir("loader_cancel");
    WriteText(dir.mPath / "scene.json", R"({"name":"scene","entities":[]})");
    mir::JobExecutor jobs(0);
    mir::SceneLoader loader(jobs);
    const auto first = mir::AllocateSceneInstanceId();
    const auto second = mir::AllocateSceneInstanceId();
    loader.Request("cancelled", dir.mPath, first);
    loader.Cancel();
    CHECK_FALSE(loader.Pending());
    CHECK_FALSE(loader.TakeReady(first).has_value());

    loader.Request("stale", dir.mPath, first);
    CHECK_FALSE(loader.TakeReady(second).has_value());
    CHECK_FALSE(loader.Pending());
    loader.Request("latest", dir.mPath, second);
    const auto completed = loader.TakeReady(second);
    REQUIRE(completed.has_value());
    CHECK(completed->id == "latest");
    CHECK(completed->prepared.probe.Ok());
}

TEST_CASE("Background scene preparation publishes only the latest request", "[scene][prepare]")
{
    TempSceneDir dir("loader_background");
    WriteText(dir.mPath / "scene.json", R"({"name":"scene","entities":[]})");
    mir::JobExecutor jobs(1);
    mir::SceneLoader loader(jobs);
    const auto source = mir::AllocateSceneInstanceId();
    loader.Request("superseded", dir.mPath, source);
    loader.Request("latest", dir.mPath, source);
    std::optional<mir::SceneLoadCompletion> completed;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!completed && std::chrono::steady_clock::now() < deadline)
    {
        completed = loader.TakeReady(source);
        if (!completed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(completed.has_value());
    CHECK(completed->id == "latest");
    CHECK(completed->prepared.probe.Ok());
    CHECK_FALSE(loader.Pending());
}

TEST_CASE("An empty scene standing in for a broken one is unbound and does not save over it", "[scene]")
{
    TempSceneDir dir("unbound_save");
    WriteBrokenScene(dir.mPath);

    World world;
    SystemScheduler scheduler;
    SceneModule module(ModuleContext{ world, scheduler }, dir.mPath);

    module.New("untitled", true);
    CHECK_FALSE(module.IsBound());
    CHECK_FALSE(module.Save(nullptr));
    CheckBrokenSceneIntact(dir.mPath);

    // the broken scene is still on disk, so a New over the untitled scene is untitled too
    module.New("untitled");
    CHECK_FALSE(module.IsBound());
    CHECK_FALSE(module.Save(nullptr));
    CheckBrokenSceneIntact(dir.mPath);
}

TEST_CASE("Bind lets an untitled scene replace the broken one", "[scene]")
{
    TempSceneDir dir("bind_then_save");
    WriteBrokenScene(dir.mPath);

    World world;
    SystemScheduler scheduler;
    SceneModule module(ModuleContext{ world, scheduler }, dir.mPath);

    module.New("untitled", true);
    module.Bind();
    REQUIRE(module.Save(nullptr));

    CHECK(mir::ProbeScene(dir.mPath).Ok());
    CHECK_FALSE(std::filesystem::exists(dir.mPath / "entities" / "1.json"));
    CHECK_FALSE(std::filesystem::exists(dir.mPath / "instances" / "2.json"));
}

TEST_CASE("Loading from disk binds an untitled scene", "[scene]")
{
    TempSceneDir dir("load_binds");

    World world;
    SystemScheduler scheduler;
    REQUIRE(mir::SaveScene(world, dir.mPath, mir::NewScene("saved")));

    SceneModule module(ModuleContext{ world, scheduler }, dir.mPath);
    CHECK(module.IsBound());

    module.New("untitled", true);
    REQUIRE_FALSE(module.IsBound());

    module.LoadFromDisk(nullptr, false);
    CHECK(module.IsBound());
    CHECK(module.Scene().mName == "saved");
    CHECK(module.Save(nullptr));
}
