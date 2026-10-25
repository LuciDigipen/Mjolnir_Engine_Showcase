/**
 * @file PrefabSave.tests.cpp
 * @author Jackey Cen
 * @brief Tests for prefab/PrefabSave.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <prefab/PrefabFormat.h>
#include <prefab/PrefabRegistry.h>
#include <prefab/PrefabSave.h>
#include <prefab/PrefabSceneInstance.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/fs/FileIO.h>
#include <scene/SceneAsset.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using mir::HasPrefabErrors;
    using mir::LoadedScene;
    using mir::NewScene;
    using mir::PlanSavePrefab;
    using mir::PlanSaveScene;
    using mir::PrefabComponentInstance;
    using mir::PrefabDocument;
    using mir::PrefabId;
    using mir::PrefabNode;
    using mir::PrefabNodeKind;
    using mir::PrefabRegistry;
    using mir::PrefabSavePlan;
    using mir::PrefabSaveResult;
    using mir::ExecuteSavePlan;
    using mir::ToString;

    const std::string &ProcessToken()
    {
        static const std::string token = []
            {
                std::random_device rd;
                std::ostringstream out;
                out << std::hex << ((static_cast<std::uint64_t>(rd()) << 32) ^ rd());
                return out.str();
            }();
        return token;
    }

    struct TempDir
    {
        std::filesystem::path mPath;

        TempDir()
        {
            static std::atomic<int> counter{ 0 };
            mPath = std::filesystem::temp_directory_path() /
                ("mir_prefab_save_test_" + ProcessToken() + "_" + std::to_string(counter.fetch_add(1)));
            std::filesystem::create_directories(mPath);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(mPath, ec);
        }

        TempDir(const TempDir &) = delete;
        TempDir &operator=(const TempDir &) = delete;
    };

    std::string ReadText(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    PrefabDocument Named(PrefabDocument document, std::string name)
    {
        document.mNodes[0].mComponents.push_back(PrefabComponentInstance{
            "Name", nlohmann::json{ { "name", std::move(name) } } });
        return document;
    }

    PrefabDocument WithDependency(PrefabDocument document, PrefabId dependency)
    {
        PrefabNode nested;
        nested.mId = 2;
        nested.mKind = PrefabNodeKind::Prefab;
        nested.mParent = 1;
        nested.mPrefab = dependency;
        document.mNodes.push_back(std::move(nested));
        document.mNextNodeId = 3;
        return document;
    }

    struct Fixture
    {
        TempDir mDir;
        PrefabRegistry mRegistry;

        Fixture() { mir::RegisterCoreComponents(); }

        void Save(const PrefabDocument &document)
        {
            REQUIRE(mir::WritePrefabFile(mDir.mPath / (ToString(document.mId) + ".prefab"), document));
        }

        void Scan() { CHECK(mRegistry.Scan(mDir.mPath).empty()); }
    };
}

TEST_CASE("Saving a prefab writes its unsaved dependencies first", "[prefab][save]")
{
    Fixture fixture;
    const PrefabDocument dependency = Named(mir::NewPrefabDocument(), "Dependency");
    const PrefabDocument root = WithDependency(mir::NewPrefabDocument(), dependency.mId);
    fixture.Save(dependency);
    fixture.Save(root);
    fixture.Scan();

    const PrefabSavePlan plan = PlanSavePrefab(fixture.mRegistry, root.mId,
        [](PrefabId) { return true; });
    REQUIRE_FALSE(HasPrefabErrors(plan.mDiagnostics));
    REQUIRE(plan.mSteps.size() == 2);
    CHECK(plan.mSteps[0].mPrefab == dependency.mId);
    CHECK(plan.mSteps[1].mPrefab == root.mId);

    const PrefabSavePlan saved = PlanSavePrefab(fixture.mRegistry, root.mId,
        [&dependency](PrefabId id) { return id != dependency.mId; });
    REQUIRE(saved.mSteps.size() == 1);
    CHECK(saved.mSteps[0].mPrefab == root.mId);
}

TEST_CASE("Saving a scene plans the unsaved prefabs its instances use", "[prefab][save]")
{
    Fixture fixture;
    const PrefabDocument dependency = Named(mir::NewPrefabDocument(), "Dependency");
    const PrefabDocument root = WithDependency(mir::NewPrefabDocument(), dependency.mId);
    fixture.Save(dependency);
    fixture.Save(root);
    fixture.Scan();

    LoadedScene scene = NewScene("scene");
    scene.mInstances[1].mPrefab = ToString(root.mId);

    const PrefabSavePlan plan = PlanSaveScene(fixture.mRegistry, scene, [](PrefabId) { return true; });
    REQUIRE_FALSE(HasPrefabErrors(plan.mDiagnostics));
    REQUIRE(plan.mSteps.size() == 2);
    CHECK(plan.mSteps[0].mPrefab == dependency.mId);
    CHECK(plan.mSteps[1].mPrefab == root.mId);
}

TEST_CASE("A write failure stops the plan and leaves the old file", "[prefab][save]")
{
    Fixture fixture;
    PrefabDocument first = Named(mir::NewPrefabDocument(), "First");
    PrefabDocument second = Named(mir::NewPrefabDocument(), "Second");
    PrefabDocument root = WithDependency(mir::NewPrefabDocument(), first.mId);
    PrefabNode other;
    other.mId = 3;
    other.mKind = PrefabNodeKind::Prefab;
    other.mParent = 1;
    other.mPrefab = second.mId;
    root.mNodes.push_back(other);
    root.mNextNodeId = 4;

    fixture.Save(first);
    fixture.Save(second);
    fixture.Save(root);
    fixture.Scan();

    const std::filesystem::path secondPath = *fixture.mRegistry.PathOf(second.mId);
    const std::filesystem::path rootPath = *fixture.mRegistry.PathOf(root.mId);
    const std::string secondBefore = ReadText(secondPath);
    const std::string rootBefore = ReadText(rootPath);

    PrefabDocument changed = first;
    changed.mNodes[0].mComponents.front().mFields["name"] = "Changed";
    REQUIRE(fixture.mRegistry.SetDocument(changed));

    std::filesystem::create_directories(secondPath.string() + ".tmp");

    const PrefabSavePlan plan = PlanSavePrefab(fixture.mRegistry, root.mId, [](PrefabId) { return true; });
    const PrefabSaveResult result = ExecuteSavePlan(fixture.mRegistry, plan);

    CHECK_FALSE(result.mSucceeded);
    REQUIRE(result.mWritten.size() == 1);
    CHECK(result.mWritten[0] == first.mId);
    CHECK(ReadText(secondPath) == secondBefore);
    CHECK(ReadText(rootPath) == rootBefore);

    const std::string firstAfter = ReadText(*fixture.mRegistry.PathOf(first.mId));
    CHECK(firstAfter.find("Changed") != std::string::npos);
}

TEST_CASE("Saving an unsaved prefab writes its intended path", "[prefab][save]")
{
    Fixture fixture;
    const PrefabDocument document = Named(mir::NewPrefabDocument(), "Intended");
    const std::filesystem::path path = fixture.mDir.mPath / "prefabs" / "Intended.prefab";
    REQUIRE(fixture.mRegistry.SetDocument(document, path));

    CHECK(fixture.mRegistry.PathOf(document.mId) == path);
    CHECK(fixture.mRegistry.IntendedPathOf(document.mId) == path);
    CHECK_FALSE(fixture.mRegistry.HasSavedFile(document.mId));
    CHECK(fixture.mRegistry.IdAtPath(path) == document.mId);

    const PrefabSavePlan plan = PlanSavePrefab(fixture.mRegistry, document.mId, [](PrefabId) { return true; });
    REQUIRE_FALSE(HasPrefabErrors(plan.mDiagnostics));
    REQUIRE(plan.mSteps.size() == 1);
    CHECK(plan.mSteps[0].mPath == path);

    const PrefabSaveResult result = ExecuteSavePlan(fixture.mRegistry, plan);
    CHECK(result.mSucceeded);
    CHECK(std::filesystem::exists(path));
    CHECK(fixture.mRegistry.HasSavedFile(document.mId));
    CHECK(fixture.mRegistry.PathOf(document.mId) == path);
}

TEST_CASE("Saving a scene writes an unsaved prefab to its intended path", "[prefab][save]")
{
    Fixture fixture;
    const PrefabDocument document = Named(mir::NewPrefabDocument(), "SceneIntended");
    const std::filesystem::path path = fixture.mDir.mPath / "prefabs" / "SceneIntended.prefab";
    REQUIRE(fixture.mRegistry.SetDocument(document, path));

    LoadedScene scene = NewScene("scene");
    scene.mInstances[1].mPrefab = ToString(document.mId);

    const PrefabSavePlan plan = PlanSaveScene(fixture.mRegistry, scene, [](PrefabId) { return true; });
    REQUIRE_FALSE(HasPrefabErrors(plan.mDiagnostics));
    REQUIRE(plan.mSteps.size() == 1);
    CHECK(plan.mSteps[0].mPath == path);

    const PrefabSaveResult result = ExecuteSavePlan(fixture.mRegistry, plan);
    CHECK(result.mSucceeded);
    CHECK(std::filesystem::exists(path));
}

TEST_CASE("A prefab without a file path is reported and nothing is written", "[prefab][save]")
{
    PrefabRegistry registry;
    const PrefabDocument document = Named(mir::NewPrefabDocument(), "Unsaved");
    REQUIRE(registry.SetDocument(document));

    const PrefabSavePlan plan = PlanSavePrefab(registry, document.mId, [](PrefabId) { return true; });
    CHECK(HasPrefabErrors(plan.mDiagnostics));
    CHECK(plan.mSteps.empty());

    const PrefabSaveResult result = ExecuteSavePlan(registry, plan);
    CHECK_FALSE(result.mSucceeded);
    CHECK(result.mWritten.empty());
}

TEST_CASE("First Prefab save rechecks path ownership after planning", "[prefab][save]")
{
    Fixture fixture;
    const PrefabDocument document = Named(mir::NewPrefabDocument(), "New");
    const PrefabDocument other = Named(mir::NewPrefabDocument(), "Other");
    const auto path = fixture.mDir.mPath / "Reserved.prefab";
    REQUIRE(fixture.mRegistry.SetDocument(document, path));
    const auto plan = PlanSavePrefab(fixture.mRegistry, document.mId, [](PrefabId) { return true; });
    REQUIRE_FALSE(HasPrefabErrors(plan.mDiagnostics));
    bool occupiedFile = true;
    SECTION("An unscanned Prefab arrives at the intended path")
    {
        REQUIRE(mir::WritePrefabFile(path, other));
    }
    SECTION("A scan registers a different Prefab at the intended path")
    {
        REQUIRE(mir::WritePrefabFile(path, other));
        fixture.Scan();
    }
    SECTION("A non-Prefab file arrives at the intended path")
    {
        REQUIRE(mir::WriteText(path, "unrelated user content"));
    }
    SECTION("Another unsaved document claims the intended path")
    {
        occupiedFile = false;
        REQUIRE(fixture.mRegistry.SetDocument(other, path));
    }
    const std::string before = occupiedFile ? ReadText(path) : std::string();
    const auto result = ExecuteSavePlan(fixture.mRegistry, plan);
    CHECK_FALSE(result.mSucceeded);
    CHECK(HasPrefabErrors(result.mDiagnostics));
    CHECK(result.mWritten.empty());
    CHECK_FALSE(fixture.mRegistry.HasSavedFile(document.mId));
    CHECK(fixture.mRegistry.IntendedPathOf(document.mId) == path);
    if (occupiedFile)
        CHECK(ReadText(path) == before);
    else
        CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("An existing file with the same Prefab UUID remains writable", "[prefab][save]")
{
    Fixture fixture;
    PrefabDocument document = Named(mir::NewPrefabDocument(), "Original");
    const auto path = fixture.mDir.mPath / "Same.prefab";
    REQUIRE(mir::WritePrefabFile(path, document));
    document.mNodes[0].mComponents.front().mFields["name"] = "Changed";
    REQUIRE(fixture.mRegistry.SetDocument(document, path));
    const auto plan = PlanSavePrefab(fixture.mRegistry, document.mId, [](PrefabId) { return true; });
    REQUIRE(ExecuteSavePlan(fixture.mRegistry, plan).mSucceeded);
    CHECK(ReadText(path).find("Changed") != std::string::npos);
}
