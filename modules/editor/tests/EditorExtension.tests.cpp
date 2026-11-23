/**
 * @file EditorExtension.tests.cpp
 * @author Jackey Cen
 * @brief Tests for editor/EditorExtension.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/ContentBrowserPaths.h>
#include <editor/ContentBrowserWatcher.h>
#include <editor/EditorDrafts.h>
#include <editor/EditorExtension.h>

#include <core/ecs/EngineMode.h>
#include <core/ecs/World.h>
#include <editor/EditorCamera.h>
#include <editor/history/History.h>
#include <project/GameProject.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using mir::BrowserAssetsRoot;
    using mir::BrowserPathForAsset;
    using mir::ContentBrowserEntryView;
    using mir::ContentBrowserWatcher;
    using mir::EditorCamera;
    using mir::EditorExtensionContext;
    using mir::EditorExtensions;
    using mir::EngineModeState;
    using mir::GameProject;
    using mir::History;
    using mir::IEditorExtension;
    using mir::kSceneDocument;
    using mir::PixelRect;
    using mir::RawInputSnapshot;
    using mir::World;

    struct Fixture
    {
        World mWorld;
        History mHistory;
        EngineModeState mMode;
        EditorCamera mCamera;
        RawInputSnapshot mInput;
        PixelRect mViewport;

        EditorExtensionContext Context()
        {
            return EditorExtensionContext{ mMode, kSceneDocument, mWorld, mViewport, mCamera, mInput, false, nullptr,
                mHistory };
        }
    };

    struct FakeExtension final : IEditorExtension
    {
        FakeExtension(std::string name, std::vector<std::string> &events)
            : mName(std::move(name)), mEvents(events)
        {
        }

        void Update(const EditorExtensionContext &) override { mEvents.push_back(mName + ".update"); }

        std::span<const char *const> WindowNames() const override
        {
            return std::span<const char *const>(mWindows);
        }

        void DrawWindows(const EditorExtensionContext &) override { mEvents.push_back(mName + ".windows"); }
        void DrawToolbar(const EditorExtensionContext &) override { mEvents.push_back(mName + ".toolbar"); }
        bool OwnsViewport() const override { return mOwnsViewport; }
        void DrawOverlay(const EditorExtensionContext &) override { mEvents.push_back(mName + ".overlay"); }

        std::span<const char *const> ContentBrowserCreateItems() const override
        {
            return std::span<const char *const>(mCreateItems);
        }

        void OnContentBrowserCreateItem(std::string_view item, std::optional<std::string_view> assetsFolder) override
        {
            mEvents.push_back(mName + ".create:" + std::string(item) + ":" +
                (assetsFolder ? std::string(*assetsFolder) : std::string("<outside>")));
        }

        bool OpenContentBrowserFile(std::string_view assetsRelativePath) override
        {
            mEvents.push_back(mName + ".open:" + std::string(assetsRelativePath));
            return mAcceptsFiles;
        }

        std::span<const char *const> UnsavedDraftNames() const override
        {
            return std::span<const char *const>(mDraftNames);
        }

        std::string mName;
        std::vector<std::string> &mEvents;
        bool mOwnsViewport = false;
        bool mAcceptsFiles = false;
        std::vector<const char *> mWindows;
        std::vector<const char *> mCreateItems;
        std::vector<const char *> mDraftNames;
    };
}

TEST_CASE("Extensions run in registration order", "[editor][extension]")
{
    Fixture fixture;
    EditorExtensions extensions;
    std::vector<std::string> events;
    FakeExtension first("first", events);
    FakeExtension second("second", events);
    const mir::Registration firstToken = extensions.Add(first);
    const mir::Registration secondToken = extensions.Add(second);

    const EditorExtensionContext context = fixture.Context();
    extensions.Update(context);
    extensions.DrawWindows(context);
    extensions.DrawToolbar(context);
    extensions.DrawOverlay(context);

    const std::vector<std::string> expected{
        "first.update", "second.update",
        "first.windows", "second.windows",
        "first.toolbar", "second.toolbar",
        "first.overlay", "second.overlay",
    };
    CHECK(events == expected);
}

TEST_CASE("A registration removes its extension when it dies", "[editor][extension]")
{
    Fixture fixture;
    EditorExtensions extensions;
    std::vector<std::string> events;
    FakeExtension first("first", events);
    FakeExtension second("second", events);
    const mir::Registration secondToken = extensions.Add(second);
    {
        const mir::Registration firstToken = extensions.Add(first);
        const EditorExtensionContext context = fixture.Context();
        extensions.Update(context);
        CHECK(events == std::vector<std::string>{ "second.update", "first.update" });
    }

    events.clear();
    const EditorExtensionContext context = fixture.Context();
    extensions.Update(context);

    CHECK(events == std::vector<std::string>{ "second.update" });
}

TEST_CASE("A moved registration keeps its extension registered", "[editor][extension]")
{
    Fixture fixture;
    EditorExtensions extensions;
    std::vector<std::string> events;
    FakeExtension first("first", events);
    FakeExtension second("second", events);
    mir::Registration token = extensions.Add(first);
    mir::Registration moved = std::move(token);
    token = extensions.Add(second);

    const EditorExtensionContext context = fixture.Context();
    extensions.Update(context);

    CHECK(events == std::vector<std::string>{ "first.update", "second.update" });
}

TEST_CASE("The viewport-owning flag hides the gizmo, shortcuts and picking", "[editor][extension]")
{
    Fixture fixture;
    EditorExtensions extensions;
    std::vector<std::string> events;
    FakeExtension ordinary("ordinary", events);
    FakeExtension owner("owner", events);
    owner.mOwnsViewport = true;

    const mir::Registration ordinaryToken = extensions.Add(ordinary);
    CHECK_FALSE(extensions.OwnsViewport());

    mir::Registration ownerToken = extensions.Add(owner);
    CHECK(extensions.OwnsViewport());

    owner.mOwnsViewport = false;
    CHECK_FALSE(extensions.OwnsViewport());

    owner.mOwnsViewport = true;
    ownerToken.Reset();
    CHECK_FALSE(extensions.OwnsViewport());
}

TEST_CASE("Window names come from every extension in registration order", "[editor][extension]")
{
    EditorExtensions extensions;
    std::vector<std::string> events;
    FakeExtension first("first", events);
    FakeExtension second("second", events);
    first.mWindows = { "First" };
    second.mWindows = { "Second", "SecondTool" };
    const mir::Registration firstToken = extensions.Add(first);
    const mir::Registration secondToken = extensions.Add(second);

    const std::vector<const char *> names = extensions.WindowNames();

    REQUIRE(names.size() == 3);
    CHECK(std::string(names[0]) == "First");
    CHECK(std::string(names[1]) == "Second");
    CHECK(std::string(names[2]) == "SecondTool");
}

TEST_CASE("Content browser create items come from every extension in registration order", "[editor][extension]")
{
    EditorExtensions extensions;
    std::vector<std::string> events;
    FakeExtension first("first", events);
    FakeExtension second("second", events);
    first.mCreateItems = { "New Widget" };
    second.mCreateItems = { "New Gizmo", "New Camera" };
    const mir::Registration firstToken = extensions.Add(first);
    const mir::Registration secondToken = extensions.Add(second);

    const std::vector<std::string> items = extensions.ContentBrowserCreateItems();

    CHECK(items == std::vector<std::string>{ "New Widget", "New Gizmo", "New Camera" });
}

TEST_CASE("A create item reaches the extension that declared it with its assets-relative folder",
    "[editor][extension]")
{
    EditorExtensions extensions;
    std::vector<std::string> events;
    FakeExtension owner("owner", events);
    FakeExtension other("other", events);
    owner.mCreateItems = { "New Widget" };
    other.mCreateItems = { "New Gizmo" };
    const mir::Registration ownerToken = extensions.Add(owner);
    const mir::Registration otherToken = extensions.Add(other);

    extensions.CreateContentBrowserItem("New Widget", "assets", "assets/models");
    CHECK(events == std::vector<std::string>{ "owner.create:New Widget:models" });

    events.clear();
    extensions.CreateContentBrowserItem("New Widget", "assets", "assets");
    CHECK(events == std::vector<std::string>{ "owner.create:New Widget:" });

    events.clear();
    extensions.CreateContentBrowserItem("New Widget", "assets", "scenes");
    CHECK(events == std::vector<std::string>{ "owner.create:New Widget:<outside>" });
}

TEST_CASE("A file open reaches the first extension that accepts it, in registration order",
    "[editor][extension]")
{
    EditorExtensions extensions;
    std::vector<std::string> events;
    FakeExtension first("first", events);
    FakeExtension second("second", events);
    FakeExtension third("third", events);
    const mir::Registration firstToken = extensions.Add(first);
    const mir::Registration secondToken = extensions.Add(second);
    const mir::Registration thirdToken = extensions.Add(third);

    CHECK_FALSE(extensions.OpenContentBrowserFile("assets", "assets/models/alpha.config"));
    CHECK(events == std::vector<std::string>{
        "first.open:models/alpha.config", "second.open:models/alpha.config", "third.open:models/alpha.config" });

    events.clear();
    second.mAcceptsFiles = true;
    CHECK(extensions.OpenContentBrowserFile("assets", "assets/models/alpha.config"));
    CHECK(events == std::vector<std::string>{
        "first.open:models/alpha.config", "second.open:models/alpha.config" });
}

TEST_CASE("A file outside the assets root is never offered to an extension", "[editor][extension]")
{
    EditorExtensions extensions;
    std::vector<std::string> events;
    FakeExtension taker("taker", events);
    taker.mAcceptsFiles = true;
    const mir::Registration token = extensions.Add(taker);

    CHECK_FALSE(extensions.OpenContentBrowserFile("assets", "assets2/models/alpha.config"));
    CHECK_FALSE(extensions.OpenContentBrowserFile("assets", "scenes/default/scene.json"));
    CHECK(events.empty());
}

TEST_CASE("The browser's entries reach an extension through the real assets root", "[editor][extension]")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "editor-extension-browser-regression";
    fs::remove_all(root);
    const auto write = [&root](const std::string &relative)
        {
            const fs::path path = root / relative;
            fs::create_directories(path.parent_path());
            std::ofstream(path) << "x";
        };
    write("data/content/models/alpha.config");
    write("data/content2/models/beta.config");
    write("scenes/default/scene.json");

    GameProject project;
    project.mDir = "games/Test";
    project.mName = "Test";
    project.mAssetsRoot = "data/content";
    project.mAssetIdRoot = "games/Test/data/content";
    project.mRootDir = root;

    ContentBrowserWatcher watcher;
    REQUIRE_NOTHROW(watcher.Initialize(project));
    const std::optional<std::string> assetsRoot = BrowserAssetsRoot(project.mDir, project.mAssetIdRoot);
    REQUIRE(assetsRoot.has_value());
    CHECK(*assetsRoot == "data/content");

    std::string folder;
    std::string file;
    for (const ContentBrowserEntryView &entry : watcher.View().mEntries)
    {
        if (entry.mRelativePath == "data/content/models")
            folder = entry.mRelativePath;
        if (entry.mRelativePath == "data/content/models/alpha.config")
            file = entry.mRelativePath;
    }
    REQUIRE_FALSE(folder.empty());
    REQUIRE_FALSE(file.empty());

    EditorExtensions extensions;
    std::vector<std::string> events;
    FakeExtension owner("owner", events);
    owner.mCreateItems = { "New Widget" };
    owner.mAcceptsFiles = true;
    const mir::Registration token = extensions.Add(owner);

    extensions.CreateContentBrowserItem("New Widget", *assetsRoot, folder);
    CHECK(events == std::vector<std::string>{ "owner.create:New Widget:models" });

    events.clear();
    CHECK(extensions.OpenContentBrowserFile(*assetsRoot, file));
    CHECK(events == std::vector<std::string>{ "owner.open:models/alpha.config" });

    events.clear();
    CHECK_FALSE(extensions.OpenContentBrowserFile(*assetsRoot, "data/content2/models/beta.config"));
    CHECK_FALSE(extensions.OpenContentBrowserFile(*assetsRoot, "scenes/default/scene.json"));
    CHECK(events.empty());

    const std::string idePath = BrowserPathForAsset(*assetsRoot, "models/alpha.config");
    CHECK(idePath == "data/content/models/alpha.config");
    CHECK(project.mRootDir / idePath == root / "data/content/models/alpha.config");
    CHECK(fs::is_regular_file(project.mRootDir / idePath));

    fs::remove_all(root);
}

TEST_CASE("The folder conversion follows the real assets root and its siblings", "[editor][extension]")
{
    using mir::AssetsRelativeFolder;
    using mir::BrowserAssetsRoot;
    using mir::BrowserPathForAsset;

    CHECK(BrowserAssetsRoot("games/Test", "games/Test/assets") == std::string("assets"));
    CHECK(BrowserAssetsRoot("games/Test", "games/Test/data/content") == std::string("data/content"));
    CHECK_FALSE(BrowserAssetsRoot("games/Test", "other/assets").has_value());
    CHECK(BrowserPathForAsset("data/content", "models/alpha.config") == "data/content/models/alpha.config");
    CHECK(BrowserPathForAsset("", "models/alpha.config") == "models/alpha.config");

    CHECK(AssetsRelativeFolder("assets", "assets") == std::string{});
    CHECK(AssetsRelativeFolder("assets", "assets/models") == std::string("models"));
    CHECK(AssetsRelativeFolder("data/content", "data/content/models") == std::string("models"));
    CHECK_FALSE(AssetsRelativeFolder("assets", "assets2/models").has_value());
    CHECK_FALSE(AssetsRelativeFolder("assets", "scenes").has_value());
    CHECK_FALSE(AssetsRelativeFolder("", "scenes").has_value());
}

TEST_CASE("Unsaved draft names come from every extension, skipping empty ones", "[editor][extension]")
{
    EditorExtensions extensions;
    std::vector<std::string> events;
    FakeExtension first("first", events);
    FakeExtension second("second", events);
    first.mDraftNames = { "Config drafts", "" };
    second.mDraftNames = { "Scene drafts" };
    const mir::Registration firstToken = extensions.Add(first);
    const mir::Registration secondToken = extensions.Add(second);

    CHECK(extensions.UnsavedDraftNames() ==
        std::vector<std::string>{ "Config drafts", "Scene drafts" });
}

TEST_CASE("The discard warning names every kind and keeps the material wording alone", "[editor][extension]")
{
    using mir::DraftListPhrase;
    using mir::DiscardDraftsWarning;

    const std::vector<std::string> materials{ "material drafts" };
    CHECK(DraftListPhrase(materials) == "material drafts");
    CHECK(DiscardDraftsWarning(materials) ==
        "Unsaved material drafts will be discarded, never saved automatically.");

    const std::vector<std::string> both{ "material drafts", "Config drafts", "Scene drafts" };
    CHECK(DraftListPhrase(both) == "material drafts, Config drafts and Scene drafts");
    CHECK(DiscardDraftsWarning(both) ==
        "Unsaved material drafts, Config drafts and Scene drafts will be discarded, "
        "never saved automatically.");

    CHECK(DiscardDraftsWarning({}).empty());
}