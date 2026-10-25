/**
 * @file PrefabAsset.tests.cpp
 * @author Jackey Cen
 * @brief Tests for prefab/PrefabAsset.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <prefab/PrefabAsset.h>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <optional>
#include <string>
#include <vector>

namespace
{
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
}

TEST_CASE("Cooked prefab payload keeps the document and nested UUIDs", "[prefab]")
{
    const mir::PrefabDocument child = mir::NewPrefabDocument();
    const mir::PrefabDocument parent = ParentOf(child);

    const std::vector<std::byte> bytes = mir::BuildPrefabAsset(parent);
    const std::optional<mir::AssetBlobView> blob = mir::ParseAssetBlob(bytes);
    REQUIRE(blob.has_value());
    CHECK(blob->header.typeTag == mir::kPrefabAssetTypeTag);
    CHECK(blob->header.contentVersion == mir::kPrefabAssetContentVersion);

    const mir::PrefabAssetParseResult parsed = mir::ReadPrefabAsset(*blob);
    CHECK_FALSE(mir::HasPrefabErrors(parsed.mDiagnostics));
    REQUIRE(parsed.mAsset.has_value());
    CHECK(parsed.mAsset->mDocument.mId == parent.mId);
    CHECK(parsed.mAsset->mDocument.mNodes.size() == parent.mNodes.size());
    REQUIRE(parsed.mAsset->mDependencies.size() == 1);
    CHECK(parsed.mAsset->mDependencies.front() == child.mId);
}

TEST_CASE("Cooked prefab payload rejects corrupt and foreign blobs", "[prefab]")
{
    const std::string corrupt = "{ not json";
    const std::vector<std::byte> corruptBytes = mir::BuildAssetBlob(mir::kPrefabAssetTypeTag,
        mir::kPrefabAssetContentVersion, std::as_bytes(std::span(corrupt)));
    const std::optional<mir::AssetBlobView> corruptBlob = mir::ParseAssetBlob(corruptBytes);
    REQUIRE(corruptBlob.has_value());
    const mir::PrefabAssetParseResult corruptResult = mir::ReadPrefabAsset(*corruptBlob);
    CHECK_FALSE(corruptResult.mAsset.has_value());
    CHECK(mir::HasPrefabErrors(corruptResult.mDiagnostics));

    const std::vector<std::byte> rawBytes = mir::BuildAssetBlob(mir::kRawAssetTypeTag,
        mir::kRawAssetContentVersion, {});
    const std::optional<mir::AssetBlobView> rawBlob = mir::ParseAssetBlob(rawBytes);
    REQUIRE(rawBlob.has_value());
    CHECK_FALSE(mir::ReadPrefabAsset(*rawBlob).mAsset.has_value());
}

TEST_CASE("Cooking an invalid document is refused", "[prefab]")
{
    mir::PrefabDocument invalid = mir::NewPrefabDocument();
    invalid.mRootNode = 42;
    CHECK_THROWS(mir::BuildPrefabAsset(invalid));
}
