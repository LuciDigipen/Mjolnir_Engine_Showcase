/**
 * @file PrefabCook.tests.cpp
 * @author Jackey Cen
 * @brief Tests for AssetCooker/PrefabCook.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "CookTestSupport.h"
#include <assets/AssetCache.h>
#include <assets/AssetManifest.h>
#include <core/fs/FileIO.h>
#include <core/platform/Process.h>
#include <prefab/PrefabAsset.h>
#include <prefab/PrefabFormat.h>
#include <prefab/PrefabRegistry.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using Json = nlohmann::json;

    struct CookRoot
    {
        fs::path mSource;
        fs::path mOut;

        CookRoot()
        {
            const auto token = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            mSource = fs::current_path() / ("prefab-cook-" + token);
            mOut = fs::path(mSource.string() + "-cooked");
            fs::create_directories(mSource / "prefabs");
        }

        ~CookRoot()
        {
            std::error_code ec;
            fs::remove_all(mSource, ec);
            fs::remove_all(mOut, ec);
        }

        CookRoot(const CookRoot &) = delete;
        CookRoot &operator=(const CookRoot &) = delete;

        int Cook() const
        {
            const std::vector<std::string> args{ "--source-dir", mSource.string(), "--id-root", "assets",
                                                 "--out",        mOut.string(),    "--result",
                                                 (mOut / "result.json").string() };
            return mir::LaunchProcess(GLTF_COOKER_EXE, args, true).value_or(-1);
        }

        std::optional<mir::AssetManifest> Manifest() const
        {
            return mir::AssetManifest::LoadFile(mOut / mir::kAssetManifestFileName);
        }
    };

    mir::PrefabDocument ParentOf(const mir::PrefabDocument &child)
    {
        mir::PrefabDocument parent = mir::NewPrefabDocument();
        mir::PrefabNode reference;
        reference.mId = *mir::AllocateNodeId(parent);
        reference.mKind = mir::PrefabNodeKind::Prefab;
        reference.mParent = parent.mRootNode;
        reference.mPrefab = child.mId;
        parent.mNodes.push_back(reference);
        return parent;
    }

    std::string BlobOf(const mir::AssetManifest &manifest, mir::AssetId id)
    {
        const mir::AssetManifestEntry *entry = manifest.Find(id);
        REQUIRE(entry != nullptr);
        return std::string(manifest.PathOf(*entry));
    }

    std::string ErrorOf(const fs::path &result)
    {
        std::ifstream input(result);
        const Json json = Json::parse(input);
        return json.value("diagnostic", Json::object()).value("message", "");
    }
}

TEST_CASE("A prefab cooks to a typed blob under its UUID AssetId", "[cooker][prefab]")
{
    CookRoot root;
    const mir::PrefabDocument child = mir::NewPrefabDocument();
    const mir::PrefabDocument parent = ParentOf(child);
    REQUIRE(mir::WritePrefabFile(root.mSource / "prefabs" / "child.prefab", child));
    REQUIRE(mir::WritePrefabFile(root.mSource / "prefabs" / "parent.prefab", parent));
    REQUIRE(root.Cook() == 0);

    const std::optional<mir::AssetManifest> manifest = root.Manifest();
    REQUIRE(manifest.has_value());
    const mir::AssetManifestEntry *entry = manifest->Find(mir::PrefabAssetId(parent.mId));
    REQUIRE(entry != nullptr);
    CHECK(entry->typeTag == mir::kPrefabAssetTypeTag);
    CHECK(entry->contentVersion == mir::kPrefabAssetContentVersion);
    CHECK(manifest->FindByPath("assets/prefabs/parent.prefab") == nullptr);

    mir::AssetCache cache(&*manifest, root.mOut);
    const mir::AssetBlobView *blob = cache.Load(mir::PrefabAssetId(parent.mId));
    REQUIRE(blob != nullptr);
    const mir::PrefabAssetParseResult parsed = mir::ReadPrefabAsset(*blob);
    REQUIRE(parsed.mAsset.has_value());
    CHECK(parsed.mAsset->mDocument.mId == parent.mId);
    REQUIRE(parsed.mAsset->mDependencies.size() == 1);
    CHECK(parsed.mAsset->mDependencies.front() == child.mId);
}

TEST_CASE("Editing a child prefab recooks only its blob", "[cooker][prefab]")
{
    CookRoot root;
    const mir::PrefabDocument child = mir::NewPrefabDocument();
    const mir::PrefabDocument parent = ParentOf(child);
    const fs::path childPath = root.mSource / "prefabs" / "child.prefab";
    REQUIRE(mir::WritePrefabFile(childPath, child));
    REQUIRE(mir::WritePrefabFile(root.mSource / "prefabs" / "parent.prefab", parent));
    REQUIRE(root.Cook() == 0);

    const std::optional<mir::AssetManifest> first = root.Manifest();
    REQUIRE(first.has_value());
    const std::string parentBlob = BlobOf(*first, mir::PrefabAssetId(parent.mId));
    const std::string childBlob = BlobOf(*first, mir::PrefabAssetId(child.mId));
    const std::optional<std::vector<std::byte>> parentBytes = mir::ReadBytes(root.mOut / parentBlob);
    REQUIRE(parentBytes.has_value());

    mir::PrefabDocument edited = child;
    mir::PrefabNode added;
    added.mId = *mir::AllocateNodeId(edited);
    added.mParent = edited.mRootNode;
    edited.mNodes.push_back(added);
    REQUIRE(mir::WritePrefabFile(childPath, edited));
    REQUIRE(root.Cook() == 0);

    const std::optional<mir::AssetManifest> second = root.Manifest();
    REQUIRE(second.has_value());
    CHECK(BlobOf(*second, mir::PrefabAssetId(parent.mId)) == parentBlob);
    CHECK(BlobOf(*second, mir::PrefabAssetId(child.mId)) != childBlob);
    CHECK(mir::ReadBytes(root.mOut / parentBlob) == parentBytes);
}

TEST_CASE("A malformed prefab fails the cook and names the file", "[cooker][prefab]")
{
    CookRoot root;
    REQUIRE(mir::WriteText(root.mSource / "prefabs" / "broken.prefab", "{ not json"));

    CHECK(root.Cook() != 0);
    CHECK(ErrorOf(root.mOut / "result.json").find("broken.prefab") != std::string::npos);
}

TEST_CASE("Two prefabs sharing a UUID fail the cook and name both files", "[cooker][prefab]")
{
    CookRoot root;
    const mir::PrefabDocument doc = mir::NewPrefabDocument();
    REQUIRE(mir::WritePrefabFile(root.mSource / "prefabs" / "one.prefab", doc));
    REQUIRE(mir::WritePrefabFile(root.mSource / "prefabs" / "two.prefab", doc));

    CHECK(root.Cook() != 0);
    const std::string message = ErrorOf(root.mOut / "result.json");
    CHECK(message.find("one.prefab") != std::string::npos);
    CHECK(message.find("two.prefab") != std::string::npos);
}

TEST_CASE("A prefab lists the prefabs it nests as dependencies", "[cooker][prefab][graph]")
{
    CookRoot root;
    const mir::PrefabDocument child = mir::NewPrefabDocument();
    const mir::PrefabDocument parent = ParentOf(child);
    REQUIRE(mir::WritePrefabFile(root.mSource / "prefabs" / "child.prefab", child));
    REQUIRE(mir::WritePrefabFile(root.mSource / "prefabs" / "parent.prefab", parent));
    REQUIRE(root.Cook() == 0);

    const std::optional<mir::AssetManifest> manifest = root.Manifest();
    REQUIRE(manifest.has_value());
    const mir::AssetManifestEntry *parentEntry = manifest->Find(mir::PrefabAssetId(parent.mId));
    const mir::AssetManifestEntry *childEntry = manifest->Find(mir::PrefabAssetId(child.mId));
    REQUIRE(parentEntry != nullptr);
    REQUIRE(childEntry != nullptr);

    const auto dependencies = manifest->Dependencies(*parentEntry);
    REQUIRE(dependencies.size() == 1);
    CHECK(dependencies[0] == mir::PrefabAssetId(child.mId));
    CHECK(manifest->Dependencies(*childEntry).empty());
}

TEST_CASE("A prefab that nests a prefab nobody cooked fails the cook", "[cooker][prefab][graph]")
{
    CookRoot root;
    const mir::PrefabDocument child = mir::NewPrefabDocument();
    REQUIRE(mir::WritePrefabFile(root.mSource / "prefabs" / "parent.prefab", ParentOf(child)));

    CHECK(root.Cook() != 0);
    CHECK_FALSE(fs::exists(root.mOut / mir::kAssetManifestFileName));
    CHECK(ErrorOf(root.mOut / "result.json").find("not cooked") != std::string::npos);
}
