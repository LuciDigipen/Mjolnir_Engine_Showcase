/**
 * @file ContentBrowserWatcher.tests.cpp
 * @author Rahul Nair
 * @brief Content browser watcher: disk-sync (#152) and mutation coverage.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/ContentBrowserWatcher.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <thread>

using namespace mir;

namespace
{
    struct TempProject
    {
        std::filesystem::path root;

        TempProject()
        {
            root = std::filesystem::temp_directory_path() /
                ("content-browser-test-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
            std::filesystem::remove_all(root);
            std::filesystem::create_directories(root / "assets");
        }

        ~TempProject() { std::filesystem::remove_all(root); }

        GameProject Project() const
        {
            GameProject project;
            project.mDir = "games/Test";
            project.mName = "Test";
            project.mAssetIdRoot = "games/Test/assets";
            project.mRootDir = root;
            return project;
        }

        void Write(const std::string &relative, const std::string &content = "x") const
        {
            const std::filesystem::path path = root / relative;
            std::filesystem::create_directories(path.parent_path());
            std::ofstream(path) << content;
        }
    };

    bool HasEntry(const ContentBrowserView &view, const std::string &relativePath)
    {
        for (const auto &entry : view.mEntries)
            if (entry.mRelativePath == relativePath)
                return true;
        return false;
    }

    // longer than the watcher's 250ms poll throttle
    void WaitForNextPoll() { std::this_thread::sleep_for(std::chrono::milliseconds(300)); }
} // namespace

TEST_CASE("ContentBrowserWatcher lists the content root on Initialize", "[app][content-browser]")
{
    TempProject temp;
    temp.Write("assets/model.glb");
    temp.Write("project_settings.json");

    ContentBrowserWatcher watcher;
    REQUIRE_NOTHROW(watcher.Initialize(temp.Project()));
    CHECK(HasEntry(watcher.View(), "assets/model.glb"));
    CHECK(HasEntry(watcher.View(), "project_settings.json"));
    CHECK(HasEntry(watcher.View(), "assets"));
}

TEST_CASE("ContentBrowserWatcher reflects an external add, delete and rename without reinitializing",
    "[app][content-browser]")
{
    TempProject temp;
    temp.Write("assets/keep.txt");

    ContentBrowserWatcher watcher;
    REQUIRE_NOTHROW(watcher.Initialize(temp.Project()));

    temp.Write("assets/added.txt");
    WaitForNextPoll();
    watcher.Update();
    CHECK(HasEntry(watcher.View(), "assets/added.txt"));

    std::filesystem::remove(temp.root / "assets" / "added.txt");
    WaitForNextPoll();
    watcher.Update();
    CHECK_FALSE(HasEntry(watcher.View(), "assets/added.txt"));

    // A rename is a same-tick remove+add with a matching (size, mtime) signature.
    std::filesystem::rename(temp.root / "assets" / "keep.txt", temp.root / "assets" / "renamed.txt");
    WaitForNextPoll();
    watcher.Update();
    CHECK(HasEntry(watcher.View(), "assets/renamed.txt"));
    CHECK_FALSE(HasEntry(watcher.View(), "assets/keep.txt"));
    CHECK(watcher.View().mLastRenamedFrom == "assets/keep.txt");
    CHECK(watcher.View().mLastRenamedTo == "assets/renamed.txt");
}

TEST_CASE("ContentBrowserWatcher reports a changed Prefab source once", "[app][content-browser]")
{
    TempProject temp;
    temp.Write("assets/prefabs/one.prefab");

    ContentBrowserWatcher watcher;
    REQUIRE_NOTHROW(watcher.Initialize(temp.Project()));
    CHECK_FALSE(watcher.ConsumePrefabSourceChange());

    temp.Write("assets/prefabs/one.prefab", "changed");
    WaitForNextPoll();
    watcher.Update();
    CHECK(watcher.ConsumePrefabSourceChange());
    CHECK_FALSE(watcher.ConsumePrefabSourceChange());

    temp.Write("assets/other.txt");
    WaitForNextPoll();
    watcher.Update();
    CHECK_FALSE(watcher.ConsumePrefabSourceChange());
}

TEST_CASE("ContentBrowserWatcher creates, renames and deletes through Handle", "[app][content-browser]")
{
    TempProject temp;

    ContentBrowserWatcher watcher;
    REQUIRE_NOTHROW(watcher.Initialize(temp.Project()));

    ContentBrowserAction create;
    create.mKind = ContentBrowserActionKind::CreateFolder;
    create.mTargetPath = "";
    create.mNewName = "levels";
    watcher.Handle(create);
    CHECK(std::filesystem::is_directory(temp.root / "levels"));
    CHECK(HasEntry(watcher.View(), "levels"));

    ContentBrowserAction rename;
    rename.mKind = ContentBrowserActionKind::Rename;
    rename.mTargetPath = "levels";
    rename.mNewName = "maps";
    watcher.Handle(rename);
    CHECK(std::filesystem::is_directory(temp.root / "maps"));
    CHECK_FALSE(std::filesystem::exists(temp.root / "levels"));

    ContentBrowserAction remove;
    remove.mKind = ContentBrowserActionKind::Delete;
    remove.mTargetPath = "maps";
    watcher.Handle(remove);
    CHECK_FALSE(std::filesystem::exists(temp.root / "maps"));
    CHECK(watcher.View().mLastError.empty());
}

TEST_CASE("ContentBrowserWatcher resolves an asset-relative logical path only under the asset root",
    "[app][content-browser]")
{
    TempProject temp;

    ContentBrowserWatcher watcher;
    REQUIRE_NOTHROW(watcher.Initialize(temp.Project()));

    const std::optional<std::string> underAssets = watcher.AssetLogicalPath("assets/models/duck.glb");
    REQUIRE(underAssets.has_value());
    CHECK(*underAssets == "games/Test/assets/models/duck.glb");

    CHECK_FALSE(watcher.AssetLogicalPath("project_settings.json").has_value());
    CHECK_FALSE(watcher.AssetLogicalPath("scenes/default/scene.json").has_value());
}

TEST_CASE("ContentBrowserWatcher throws a System SetupError when the game directory is gone", "[app][content-browser]")
{
    GameProject project;
    project.mDir = "games/Missing";
    project.mRootDir = std::filesystem::temp_directory_path() / "content-browser-test-missing-root";
    std::filesystem::remove_all(project.mRootDir);

    ContentBrowserWatcher watcher;
    try
    {
        watcher.Initialize(project);
        FAIL("Initialize did not throw");
    }
    catch (const SetupError &error)
    {
        CHECK(error.Category() == ErrorCategory::System);
        CHECK(error.Path() == project.mRootDir);
    }
}

TEST_CASE("ContentBrowserWatcher hides tooling files and keeps listing assets", "[app][content-browser][meta]")
{
    TempProject temp;
    temp.Write("assets/white.png");
    temp.Write("assets/white.png.meta");
    temp.Write("assets/duck.glb.import.json");
    temp.Write("assets/manifest.blob.pending");
    temp.Write("assets/data/.gitkeep");

    ContentBrowserWatcher watcher;
    REQUIRE_NOTHROW(watcher.Initialize(temp.Project()));
    CHECK(HasEntry(watcher.View(), "assets/white.png"));
    CHECK(HasEntry(watcher.View(), "assets/data"));
    CHECK_FALSE(HasEntry(watcher.View(), "assets/white.png.meta"));
    CHECK_FALSE(HasEntry(watcher.View(), "assets/duck.glb.import.json"));
    CHECK_FALSE(HasEntry(watcher.View(), "assets/manifest.blob.pending"));
    CHECK_FALSE(HasEntry(watcher.View(), "assets/data/.gitkeep"));

    // one written later stays hidden too
    temp.Write("assets/other.png.meta");
    WaitForNextPoll();
    watcher.Update();
    CHECK_FALSE(HasEntry(watcher.View(), "assets/other.png.meta"));
}

TEST_CASE("ContentBrowserWatcher renames and deletes a file with its .meta", "[app][content-browser][meta]")
{
    TempProject temp;
    temp.Write("assets/old.png");
    temp.Write("assets/old.png.meta", "guid");
    temp.Write("assets/gone.png");
    temp.Write("assets/gone.png.meta", "guid");

    ContentBrowserWatcher watcher;
    REQUIRE_NOTHROW(watcher.Initialize(temp.Project()));

    ContentBrowserAction rename;
    rename.mKind = ContentBrowserActionKind::Rename;
    rename.mTargetPath = "assets/old.png";
    rename.mNewName = "new.png";
    watcher.Handle(rename);
    CHECK(std::filesystem::exists(temp.root / "assets" / "new.png"));
    CHECK(std::filesystem::exists(temp.root / "assets" / "new.png.meta"));
    CHECK_FALSE(std::filesystem::exists(temp.root / "assets" / "old.png.meta"));

    ContentBrowserAction remove;
    remove.mKind = ContentBrowserActionKind::Delete;
    remove.mTargetPath = "assets/gone.png";
    watcher.Handle(remove);
    CHECK_FALSE(std::filesystem::exists(temp.root / "assets" / "gone.png"));
    CHECK_FALSE(std::filesystem::exists(temp.root / "assets" / "gone.png.meta"));
}

TEST_CASE("ContentBrowserWatcher moves the .meta of a file renamed outside the editor", "[app][content-browser][meta]")
{
    TempProject temp;
    temp.Write("assets/keep.png");
    temp.Write("assets/keep.png.meta", "guid");

    ContentBrowserWatcher watcher;
    REQUIRE_NOTHROW(watcher.Initialize(temp.Project()));

    std::filesystem::rename(temp.root / "assets" / "keep.png", temp.root / "assets" / "renamed.png");
    WaitForNextPoll();
    watcher.Update();
    CHECK(watcher.View().mLastRenamedTo == "assets/renamed.png");
    CHECK(std::filesystem::exists(temp.root / "assets" / "renamed.png.meta"));
    CHECK_FALSE(std::filesystem::exists(temp.root / "assets" / "keep.png.meta"));
}

TEST_CASE("ContentBrowserWatcher keeps the .meta files inside a renamed folder", "[app][content-browser][meta]")
{
    TempProject temp;
    temp.Write("assets/textures/a.png");
    temp.Write("assets/textures/a.png.meta", "guid");

    ContentBrowserWatcher watcher;
    REQUIRE_NOTHROW(watcher.Initialize(temp.Project()));

    ContentBrowserAction rename;
    rename.mKind = ContentBrowserActionKind::Rename;
    rename.mTargetPath = "assets/textures";
    rename.mNewName = "images";
    watcher.Handle(rename);
    CHECK(std::filesystem::exists(temp.root / "assets" / "images" / "a.png.meta"));
}
