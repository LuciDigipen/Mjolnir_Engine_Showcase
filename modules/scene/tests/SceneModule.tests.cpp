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

#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

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
