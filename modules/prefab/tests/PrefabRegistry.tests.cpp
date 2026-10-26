/**
 * @file PrefabRegistry.tests.cpp
 * @author Jackey Cen
 * @brief Tests for prefab/PrefabRegistry.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <prefab/PrefabRegistry.h>

#include <assets/AssetCache.h>
#include <assets/AssetManifest.h>
#include <core/fs/FileIO.h>
#include <prefab/PrefabAsset.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <fstream>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using mir::PrefabDocument;
    using mir::PrefabDiagnostic;
    using mir::PrefabDiagnosticSeverity;
    using mir::PrefabId;
    using mir::PrefabNode;
    using mir::PrefabRegistry;

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
                    ("mir_prefab_registry_test_" + ProcessToken() + "_" + std::to_string(counter.fetch_add(1)));
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

    bool HasErrorContaining(const std::vector<PrefabDiagnostic> &diagnostics, std::string_view needle)
    {
        for (const PrefabDiagnostic &diagnostic : diagnostics)
        {
            if (diagnostic.mSeverity == PrefabDiagnosticSeverity::Error &&
                diagnostic.mMessage.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }

    bool HasErrors(const std::vector<PrefabDiagnostic> &diagnostics)
    {
        return mir::HasPrefabErrors(diagnostics);
    }

    std::filesystem::path WriteDoc(TempDir &dir, std::string_view name, const PrefabDocument &doc)
    {
        const std::filesystem::path path = dir.mPath / name;
        std::filesystem::create_directories(path.parent_path());
        REQUIRE(mir::WritePrefabFile(path, doc));
        return path;
    }

    PrefabDocument DocWithChild()
    {
        PrefabDocument doc = mir::NewPrefabDocument();
        PrefabNode child;
        child.mId = *mir::AllocateNodeId(doc);
        child.mParent = doc.mRootNode;
        doc.mNodes.push_back(child);
        return doc;
    }

    void WriteCookedPrefab(TempDir &dir, std::string_view name, const PrefabDocument &doc)
    {
        REQUIRE(mir::WriteBytes(dir.mPath / name, mir::BuildPrefabAsset(doc)));
    }

    mir::AssetManifestSourceEntry CookedEntry(const PrefabDocument &doc, std::string_view file)
    {
        return { { mir::PrefabAssetId(doc.mId), mir::kPrefabAssetTypeTag, mir::kPrefabAssetContentVersion, 0, 0 },
            file };
    }

    std::optional<mir::AssetManifest> WriteManifest(TempDir &dir,
        std::span<const mir::AssetManifestSourceEntry> entries)
    {
        REQUIRE(mir::WriteBytes(dir.mPath / mir::kAssetManifestFileName, mir::BuildAssetManifestBlob(entries)));
        return mir::AssetManifest::LoadFile(dir.mPath / mir::kAssetManifestFileName);
    }
}

TEST_CASE("Scan loads prefab files recursively", "[prefab]")
{
    TempDir dir;
    const PrefabDocument first = mir::NewPrefabDocument();
    const PrefabDocument second = mir::NewPrefabDocument();
    const std::filesystem::path firstPath = WriteDoc(dir, "first.prefab", first);
    WriteDoc(dir, "nested/second.prefab", second);

    PrefabRegistry registry;
    const std::vector<PrefabDiagnostic> diagnostics = registry.Scan(dir.mPath);
    REQUIRE(diagnostics.empty());

    REQUIRE(registry.Find(first.mId) != nullptr);
    REQUIRE(registry.Find(second.mId) != nullptr);
    REQUIRE(registry.Find(mir::GeneratePrefabId()) == nullptr);
    REQUIRE(registry.PathOf(first.mId) == firstPath);
    REQUIRE(registry.PathOf(mir::GeneratePrefabId()) == std::nullopt);
}

TEST_CASE("Malformed files only add diagnostics", "[prefab]")
{
    TempDir dir;
    const PrefabDocument good = mir::NewPrefabDocument();
    WriteDoc(dir, "good.prefab", good);
    {
        std::ofstream file(dir.mPath / "bad.prefab");
        file << "{ not json";
    }

    PrefabRegistry registry;
    const std::vector<PrefabDiagnostic> diagnostics = registry.Scan(dir.mPath);
    REQUIRE(HasErrors(diagnostics));
    bool namesBadFile = false;
    for (const PrefabDiagnostic &diagnostic : diagnostics)
        namesBadFile = namesBadFile || diagnostic.mPath.find("bad.prefab") != std::string::npos;
    REQUIRE(namesBadFile);
    REQUIRE(registry.Find(good.mId) != nullptr);
}

TEST_CASE("In-memory documents resolve before disk", "[prefab]")
{
    TempDir dir;
    const PrefabDocument disk = mir::NewPrefabDocument();
    WriteDoc(dir, "disk.prefab", disk);

    PrefabRegistry registry;
    REQUIRE(registry.Scan(dir.mPath).empty());
    REQUIRE(registry.Find(disk.mId)->mNodes.size() == 1);

    const PrefabDocument memory = DocWithChild();
    PrefabDocument unsaved = disk;
    unsaved.mNodes = memory.mNodes;
    unsaved.mNextNodeId = memory.mNextNodeId;
    REQUIRE(registry.SetDocument(unsaved));
    REQUIRE(registry.Find(disk.mId)->mNodes.size() == 2);

    REQUIRE(registry.RemoveDocument(disk.mId));
    REQUIRE(registry.Find(disk.mId)->mNodes.size() == 1);
    REQUIRE_FALSE(registry.RemoveDocument(disk.mId));
}

TEST_CASE("Invalid documents never replace registry state", "[prefab]")
{
    PrefabRegistry registry;

    const PrefabDocument valid = mir::NewPrefabDocument();
    REQUIRE(registry.SetDocument(valid));

    PrefabDocument invalid = valid;
    invalid.mRootNode = 99;
    std::vector<PrefabDiagnostic> diagnostics;
    REQUIRE_FALSE(registry.SetDocument(invalid, &diagnostics));
    REQUIRE(HasErrors(diagnostics));

    const PrefabDocument *stored = registry.Find(valid.mId);
    REQUIRE(stored != nullptr);
    REQUIRE(stored->mRootNode == valid.mRootNode);
}

TEST_CASE("Duplicate UUIDs on disk resolve from no file", "[prefab]")
{
    TempDir dir;
    const PrefabDocument doc = mir::NewPrefabDocument();
    WriteDoc(dir, "a.prefab", doc);
    WriteDoc(dir, "b.prefab", doc);

    PrefabRegistry registry;
    const std::vector<PrefabDiagnostic> diagnostics = registry.Scan(dir.mPath);
    REQUIRE(HasErrors(diagnostics));
    REQUIRE(HasErrorContaining(diagnostics, "a.prefab"));
    REQUIRE(HasErrorContaining(diagnostics, "b.prefab"));
    REQUIRE(HasErrorContaining(diagnostics, mir::ToString(doc.mId)));

    REQUIRE(registry.Find(doc.mId) == nullptr);
    REQUIRE(registry.PathOf(doc.mId) == std::nullopt);
}

TEST_CASE("Renaming a prefab keeps its identity", "[prefab]")
{
    TempDir dir;
    const PrefabDocument doc = DocWithChild();
    const std::filesystem::path oldPath = WriteDoc(dir, "old.prefab", doc);

    PrefabRegistry registry;
    REQUIRE(registry.Scan(dir.mPath).empty());
    REQUIRE(registry.PathOf(doc.mId) == oldPath);

    const std::filesystem::path newPath = dir.mPath / "moved.prefab";
    std::filesystem::rename(oldPath, newPath);
    REQUIRE(registry.Scan(dir.mPath).empty());

    const PrefabDocument *found = registry.Find(doc.mId);
    REQUIRE(found != nullptr);
    REQUIRE(registry.PathOf(doc.mId) == newPath);
    REQUIRE(found->mId == doc.mId);
    REQUIRE(found->mNextNodeId == doc.mNextNodeId);
    REQUIRE(found->mNodes.size() == doc.mNodes.size());
    REQUIRE(found->mNodes[1].mId == doc.mNodes[1].mId);
}

TEST_CASE("CopyAsNew forks identity but keeps nodes", "[prefab]")
{
    PrefabRegistry registry;
    const PrefabDocument doc = DocWithChild();
    REQUIRE(registry.SetDocument(doc));

    const std::optional<PrefabDocument> copy = registry.CopyAsNew(doc);
    REQUIRE(copy.has_value());
    REQUIRE(copy->mId != doc.mId);
    REQUIRE(registry.Find(copy->mId) == nullptr);
    REQUIRE(copy->mNodes.size() == doc.mNodes.size());
    REQUIRE(copy->mNextNodeId == doc.mNextNodeId);
    REQUIRE(copy->mRootNode == doc.mRootNode);

    REQUIRE(registry.SetDocument(*copy));
    REQUIRE(registry.Find(doc.mId) != nullptr);
    REQUIRE(registry.Find(copy->mId) != nullptr);

    PrefabDocument invalid = doc;
    invalid.mRootNode = 99;
    REQUIRE(registry.CopyAsNew(invalid) == std::nullopt);
}

TEST_CASE("ResolveAssetId adapts UUIDs and checks collisions", "[prefab]")
{
    PrefabRegistry registry;
    const PrefabDocument doc = mir::NewPrefabDocument();
    REQUIRE(registry.SetDocument(doc));

    REQUIRE(mir::PrefabAssetId(mir::kNullPrefabId).IsNull());

    const mir::AssetId expected = mir::MakeAssetId("prefab:" + mir::ToString(doc.mId));
    REQUIRE(mir::PrefabAssetId(doc.mId) == expected);
    REQUIRE_FALSE(expected.IsNull());

    const std::optional<mir::AssetId> resolved = registry.ResolveAssetId(doc.mId);
    REQUIRE(resolved.has_value());
    REQUIRE(*resolved == expected);

    REQUIRE(registry.ResolveAssetId(mir::GeneratePrefabId()) == std::nullopt);

    const auto makeManifest = [](mir::AssetId id)
        {
            mir::AssetManifestEntry entry{};
            entry.id = id;
            entry.typeTag = 1;
            entry.contentVersion = 1;
            mir::AssetManifestSourceEntry source;
            source.entry = entry;
            source.path = "models/rock.model";
            const std::array<mir::AssetManifestSourceEntry, 1> entries{ source };
            return mir::AssetManifest::Parse(mir::BuildAssetManifestBlob(entries));
        };

    SECTION("manifest owns the hash")
    {
        const std::optional<mir::AssetManifest> manifest = makeManifest(expected);
        REQUIRE(manifest.has_value());

        std::vector<PrefabDiagnostic> diagnostics;
        REQUIRE(registry.ResolveAssetId(doc.mId, &*manifest, &diagnostics) == std::nullopt);
        REQUIRE(HasErrorContaining(diagnostics, "models/rock.model"));
        REQUIRE(HasErrorContaining(diagnostics, mir::ToString(doc.mId)));
    }
    SECTION("manifest without the hash is no collision")
    {
        const std::optional<mir::AssetManifest> manifest = makeManifest(mir::MakeAssetId("models/other.model"));
        REQUIRE(manifest.has_value());
        REQUIRE(registry.ResolveAssetId(doc.mId, &*manifest) == expected);
    }
}

TEST_CASE("Cooked prefabs fill the disk table without source paths", "[prefab]")
{
    TempDir dir;
    const PrefabDocument doc = DocWithChild();
    WriteCookedPrefab(dir, "hero.blob", doc);
    const std::array<mir::AssetManifestSourceEntry, 1> entries{ CookedEntry(doc, "hero.blob") };
    const std::optional<mir::AssetManifest> manifest = WriteManifest(dir, entries);
    REQUIRE(manifest.has_value());
    mir::AssetCache cache(&*manifest, dir.mPath);

    PrefabRegistry registry;
    CHECK(registry.LoadCooked(*manifest, cache).empty());
    REQUIRE(registry.Find(doc.mId) != nullptr);
    CHECK(registry.Find(doc.mId)->mNodes.size() == doc.mNodes.size());
    CHECK(registry.PathOf(doc.mId) == std::nullopt);
}

TEST_CASE("Cooked prefab bad blobs are diagnosed and skipped", "[prefab]")
{
    TempDir dir;
    const PrefabDocument good = mir::NewPrefabDocument();
    const PrefabDocument bad = mir::NewPrefabDocument();
    WriteCookedPrefab(dir, "good.blob", good);
    const std::string corrupt = "not a prefab";
    REQUIRE(mir::WriteBytes(dir.mPath / "bad.blob", mir::BuildAssetBlob(mir::kPrefabAssetTypeTag,
        mir::kPrefabAssetContentVersion, std::as_bytes(std::span(corrupt)))));
    const std::array<mir::AssetManifestSourceEntry, 2> entries{ CookedEntry(good, "good.blob"),
        CookedEntry(bad, "bad.blob") };
    const std::optional<mir::AssetManifest> manifest = WriteManifest(dir, entries);
    REQUIRE(manifest.has_value());
    mir::AssetCache cache(&*manifest, dir.mPath);

    PrefabRegistry registry;
    const std::vector<PrefabDiagnostic> diagnostics = registry.LoadCooked(*manifest, cache);
    CHECK(HasErrors(diagnostics));
    bool namesBadBlob = false;
    for (const PrefabDiagnostic &diagnostic : diagnostics)
        namesBadBlob = namesBadBlob || diagnostic.mPath.find("bad.blob") != std::string::npos;
    CHECK(namesBadBlob);
    REQUIRE(registry.Find(good.mId) != nullptr);
    CHECK(registry.Find(bad.mId) == nullptr);
}
