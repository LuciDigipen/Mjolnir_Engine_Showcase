/**
 * @file ContentThumbnails.tests.cpp
 * @author sumin.park
 * @brief Preview filtering, visibility, cache eviction and reload regression coverage.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/ContentThumbnailCache.h>
#include <editor/EditorPreferences.h>
#include <editor/TextureThumbnailDecoder.h>
#include <editor/panels/ContentBrowserPanel.h>

#include <assets/AssetsModule.h>
#include <assets/ModelAsset.h>
#include <jobs/JobExecutor.h>

#include <imgui.h>
#include <imgui_internal.h>

#include "../../renderer/tests/support/CookedMeshDir.h"
#include "../../renderer/tests/support/FakeTextureUploader.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace mir;

namespace
{
    struct CookedTextures : test::CookedDir
    {
        CookedTextures() { Write(80); }
        void Write(uint8_t value)
        {
            TextureAsset texture;
            texture.width = 256;
            texture.height = 128;
            texture.rgba.assign(size_t(texture.width) * texture.height * 4, value);
            for (size_t i = 3; i < texture.rgba.size(); i += 4)
                texture.rgba[i] = 255;
            const std::string file = "texture-" + std::to_string(value) + ".blob";
            REQUIRE(WriteBytes(Path() / file, BuildTypedAsset(texture)));
            std::vector<AssetManifestSourceEntry> entries;
            for (const char *name : { "a", "b", "c" })
                entries.push_back({ { MakeAssetId(name), kTextureAssetTypeTag, kTextureAssetContentVersion }, file, name, {} });
            REQUIRE(WriteBytes(Path() / kAssetManifestFileName, BuildAssetManifestBlob(entries)));
        }
    };

    struct Fixture : CookedTextures
    {
        Fixture() : mAssets(Path()), mJobs(0), mService(mAssets, mJobs, 1), mCache(&mService, mUploader, &mJobs) {}
        void Frame(std::initializer_list<const char *> visible, size_t capacity = 2)
        {
            mService.DrainCompletions();
            mCache.BeginFrame(capacity);
            for (const auto *name : visible)
                mCache.Request(MakeAssetId(name));
            mCache.EndFrame();
        }
        TextureHandle Ready(const char *name)
        {
            Frame({name});
            Frame({name});
            REQUIRE_FALSE(mUploader.mEntries.empty());
            TextureHandle handle{};
            for (const auto &[index, entry] : mUploader.mEntries)
                if (entry.mState == TextureUploadState::Pending)
                    handle = {index, 1};
            REQUIRE_FALSE(handle.IsNull());
            mUploader.Complete(handle);
            Frame({name});
            return handle;
        }
        AssetsModule mAssets;
        JobExecutor mJobs;
        AsyncAssetService mService;
        test::FakeTextureUploader mUploader;
        ContentThumbnailCache mCache;
    };

    TextureUploadSource Decode(TextureAsset texture)
    {
        const auto bytes = BuildTypedAsset(texture);
        const auto blob = ParseAssetBlob(bytes);
        REQUIRE(blob);
        std::string error;
        TextureUploadSource out;
        REQUIRE(TextureThumbnailDecoder().decode(*blob, AssetDependencies({}), out, error));
        return out;
    }
}

TEST_CASE("Thumbnail decoder bounds dimensions without retaining full texture pixels", "[editor][thumbnails]")
{
    TextureAsset texture;
    texture.width = 4096;
    texture.height = 2048;
    texture.rgba.assign(size_t(texture.width) * texture.height * 4, 255);
    const auto out = Decode(std::move(texture));
    CHECK(out.desc.width == 128);
    CHECK(out.desc.height == 64);
    CHECK(out.Bytes() == 128 * 64 * 4);
    CHECK(out.desc.mips.empty());
    CHECK_FALSE(out.desc.generateMips);
    REQUIRE(out.keepalive);
}

TEST_CASE("Thumbnail filtering uses linear light and alpha weighting", "[editor][thumbnails]")
{
    TextureAsset texture;
    texture.width = 256;
    texture.height = 1;
    for (size_t i = 0; i < 128; ++i)
        texture.rgba.insert(texture.rgba.end(), {0, 0, 0, 255, 255, 255, 255, 255});
    auto out = Decode(texture);
    CHECK(std::to_integer<int>(out.desc.pixels[0]) == 188);
    texture.rgba.clear();
    for (size_t i = 0; i < 128; ++i)
        texture.rgba.insert(texture.rgba.end(), {255, 0, 0, 0, 0, 0, 255, 255});
    out = Decode(texture);
    CHECK(std::to_integer<int>(out.desc.pixels[0]) == 0);
    CHECK(std::to_integer<int>(out.desc.pixels[2]) == 255);
    CHECK(std::to_integer<int>(out.desc.pixels[3]) == 128);
}

TEST_CASE("Thumbnail decoder preserves small images and selects cooked mips", "[editor][thumbnails]")
{
    TextureAsset texture;
    texture.width = 4;
    texture.height = 2;
    texture.rgba.assign(32, 255);
    auto out = Decode(texture);
    CHECK(out.desc.width == 4);
    CHECK(out.desc.height == 2);
    texture.width = texture.height = 256;
    texture.rgba.assign(256 * 256 * 4, 0);
    for (uint32_t size = 128; size != 0; size /= 2)
    {
        const auto offset = static_cast<uint32_t>(texture.mipData.size());
        texture.mips.push_back({ size, size, offset, size * size * 4 });
        texture.mipData.resize(offset + size * size * 4, 255);
    }
    out = Decode(texture);
    CHECK(out.desc.width == 128);
    CHECK(std::to_integer<int>(out.desc.pixels[0]) == 255);
}

TEST_CASE("Hidden thumbnail completions never upload and list mode keeps ready previews", "[editor][thumbnails]")
{
    Fixture f;
    f.Frame({"a"});
    f.Frame({});
    CHECK(f.mUploader.mQueued == 0);
    CHECK(f.mCache.Size() == 0);
    const auto handle = f.Ready("b");
    f.Frame({});
    CHECK(f.mUploader.TextureState(handle) == TextureUploadState::Ready);
    CHECK(f.mCache.Size() == 1);
    f.Frame({}, 0);
    CHECK(f.mCache.Size() == 0);
    CHECK(f.mUploader.mRetired.size() == 1);
}

TEST_CASE("Thumbnail cache evicts least recently used and protects visible capacity", "[editor][thumbnails]")
{
    Fixture f;
    const auto a = f.Ready("a");
    const auto b = f.Ready("b");
    f.Frame({"a", "b"});
    f.Frame({"a"});
    f.Frame({"a", "c"});
    CHECK(f.mUploader.TextureState(a) == TextureUploadState::Ready);
    CHECK(f.mUploader.TextureState(b) == TextureUploadState::Stale);
    CHECK(f.mCache.Size() == 2);
    f.Frame({"a", "b", "c"}, 1);
    CHECK(f.mCache.Size() == 1);
    CHECK(f.mUploader.TextureState(a) == TextureUploadState::Ready);
}

TEST_CASE("Thumbnail failure is remembered and changed content retires its old preview", "[editor][thumbnails]")
{
    Fixture f;
    f.mUploader.mReject = true;
    f.Frame({"a"});
    f.Frame({"a"});
    CHECK(f.mCache.Size() == 0);
    f.mUploader.mReject = false;
    f.Frame({"a"});
    f.Frame({"a"});
    CHECK(f.mUploader.mQueued == 0);
    f.Write(100);
    f.mAssets.BeginReload();
    f.mAssets.CommitReload();
    const auto handle = f.Ready("a");
    f.Write(120);
    f.mAssets.BeginReload();
    f.mAssets.CommitReload();
    f.Frame({"a"});
    CHECK(f.mUploader.TextureState(handle) == TextureUploadState::Stale);
}

TEST_CASE("Content browser preferences round trip with local defaults", "[editor][thumbnails]")
{
    test::CookedDir dir;
    const auto path = dir.Path() / "preferences.json";
    EditorPreferences preferences;
    CHECK_FALSE(preferences.contentGrid);
    CHECK(preferences.contentThumbnailCapacity == 256);
    preferences.contentGrid = true;
    preferences.contentThumbnailSize = 112;
    preferences.contentThumbnailCapacity = 32;
    REQUIRE(preferences.SaveFile(path));
    const auto loaded = EditorPreferences::LoadFile(path);
    CHECK(loaded.contentGrid);
    CHECK(loaded.contentThumbnailSize == 112);
    CHECK(loaded.contentThumbnailCapacity == 32);
}

TEST_CASE("Content browser requests only visible grid textures and no list previews", "[editor][thumbnails]")
{
    struct Context
    {
        Context()
        {
            ImGui::CreateContext();
            auto &io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(800, 600);
            io.DeltaTime = 1.0f / 60;
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        }
        ~Context() { ImGui::DestroyContext(); }
    } context;
    ContentBrowserPanel panel;
    EditorPreferences preferences;
    ContentBrowserView view;
    view.mEntries.push_back({ "script.lua", "script.lua" });
    for (int i = 0; i < 200; ++i)
    {
        const auto name = "image-" + std::to_string(i) + ".png";
        view.mEntries.push_back({name, name});
    }
    size_t requests = 0;
    std::vector<std::string> requested;
    ImGuiWindow *files = nullptr;
    view.mPreview = [&](const std::string &path)
        {
            CHECK(path.ends_with(".png"));
            ++requests;
            requested.push_back(path);
            return ContentBrowserPreview{};
        };
    const auto draw = [&]()
        {
            requests = 0;
            requested.clear();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(700, 350));
            ImGui::Begin("browser");
            panel.Draw(view, preferences);
            for (auto *child : ImGui::GetCurrentWindow()->DC.ChildWindows)
                if (std::string_view(child->Name).find("ContentBrowserFiles") != std::string_view::npos)
                    files = child;
            ImGui::End();
            ImGui::Render();
        };
    draw();
    draw();
    CHECK(requests == 0);
    preferences.contentGrid = true;
    draw();
    draw();
    CHECK(requests > 0);
    CHECK(requests < 200);
    REQUIRE(files);
    const auto first = requested.front();
    ImGui::SetScrollY(files, files->ScrollMax.y);
    draw();
    draw();
    CHECK(requests > 0);
    CHECK(requests < 200);
    CHECK(std::find(requested.begin(), requested.end(), first) == requested.end());
    preferences.contentGrid = false;
    draw();
    CHECK(requests == 0);
}

TEST_CASE("Material source images preview on the editor lane without a standalone cooked texture", "[editor][thumbnails]")
{
    Fixture f;
    const auto image = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path() /
        "assets/ui/icon.png";
    REQUIRE(std::filesystem::is_regular_file(image));
    const auto id = MakeAssetId("editor-thumbnail:source.png");
    f.mCache.BeginFrame(2);
    f.mCache.Request(id, image);
    f.mCache.EndFrame();
    REQUIRE(f.mUploader.mQueued == 1);
    const auto &upload = f.mUploader.mEntries.begin()->second.mSource;
    CHECK(upload.desc.width <= 128);
    CHECK(upload.desc.height <= 128);
    CHECK(upload.Bytes() <= 128 * 128 * 4);
    // Leaving view retires the pending upload, the source pixels are not retained by the cache.
    f.Frame({});
    CHECK(f.mCache.Size() == 0);
    CHECK(f.mUploader.mRetired.size() == 1);
}
