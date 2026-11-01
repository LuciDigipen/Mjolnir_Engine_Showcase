/**
 * @file AssetManifest.tests.cpp
 * @author Rahul Nair
 * @brief Tests for assets/AssetManifest: build/parse round trip.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <assets/AssetManifest.h>

#include <assets/AssetBlob.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace
{
    // Patches the body and re-wraps it so the hash still matches
    template <class Patch>
    std::vector<std::byte> Rewrapped(const std::vector<std::byte> &blob, Patch patch)
    {
        const std::optional<mir::AssetBlobView> view = mir::ParseAssetBlob(blob);
        REQUIRE(view.has_value());
        std::vector<std::byte> content(view->content.begin(), view->content.end());
        patch(content);
        return mir::BuildAssetBlob(mir::kAssetManifestTypeTag, mir::kAssetManifestContentVersion, content);
    }

    constexpr std::size_t kEntriesStart = 2 * sizeof(uint64_t);

    // A has B and C, B and C have D, D points back at A, C also has E
    struct Graph
    {
        mir::AssetId a = mir::MakeAssetId("g/a"), b = mir::MakeAssetId("g/b"), c = mir::MakeAssetId("g/c"),
                     d = mir::MakeAssetId("g/d"), e = mir::MakeAssetId("g/e");
        std::vector<mir::AssetId> aDeps{ b, c }, bDeps{ d }, cDeps{ d, e }, dDeps{ a };

        std::vector<mir::AssetManifestSourceEntry> Entries() const
        {
            return {
                { { a, 1, 1 }, "a.blob", "g/a", aDeps },
                { { b, 2, 1 }, "b.blob", "", bDeps },
                { { c, 3, 1 }, "c.blob", "", cDeps },
                { { d, 4, 1 }, "d.blob", "", dDeps },
                { { e, 4, 1 }, "e.blob", "", {} },
            };
        }
    };
}

TEST_CASE("AssetManifest build and parse round trip", "[assets][manifest]")
{
    const mir::AssetId first = mir::MakeAssetId("a/one.raw");
    const mir::AssetId second = mir::MakeAssetId("a/two.raw");

    const mir::AssetManifestSourceEntry entries[]{
        {{first, 1, 1}, "cooked/one.blob"},
        {{second, 2, 1}, "cooked/two.blob"},
    };

    const std::vector<std::byte> blob = mir::BuildAssetManifestBlob(entries);
    const std::optional<mir::AssetManifest> manifest = mir::AssetManifest::Parse(blob);
    REQUIRE(manifest.has_value());
    CHECK(manifest->Count() == 2);

    const mir::AssetManifestEntry *foundFirst = manifest->Find(first);
    REQUIRE(foundFirst != nullptr);
    CHECK(foundFirst->typeTag == 1);
    CHECK(manifest->PathOf(*foundFirst) == "cooked/one.blob");

    const mir::AssetManifestEntry *foundSecond = manifest->Find(second);
    REQUIRE(foundSecond != nullptr);
    CHECK(foundSecond->typeTag == 2);
    CHECK(manifest->PathOf(*foundSecond) == "cooked/two.blob");
}

TEST_CASE("AssetManifest Find returns nullptr for an unknown id", "[assets][manifest]")
{
    const std::vector<std::byte> blob = mir::BuildAssetManifestBlob({});
    const std::optional<mir::AssetManifest> manifest = mir::AssetManifest::Parse(blob);
    REQUIRE(manifest.has_value());
    CHECK(manifest->Find(mir::MakeAssetId("missing")) == nullptr);
}

TEST_CASE("AssetManifest Parse rejects a blob with the wrong type tag", "[assets][manifest]")
{
    const std::vector<std::byte> blob = mir::BuildAssetBlob(999, 1, {});
    CHECK_FALSE(mir::AssetManifest::Parse(blob).has_value());
}

TEST_CASE("AssetManifest rejects an unsupported content version", "[assets][manifest]")
{
    const mir::AssetManifestSourceEntry entries[]{
        {{mir::MakeAssetId("a/one.raw"), 1, 1}, "cooked/one.blob"},
    };

    // a well-formed manifest body, re-wrapped with a content version this build
    // does not know: the entry layout it describes may not be the current one
    const std::vector<std::byte> current = mir::BuildAssetManifestBlob(entries);
    const std::optional<mir::AssetBlobView> view = mir::ParseAssetBlob(current);
    REQUIRE(view.has_value());

    const std::vector<std::byte> future = mir::BuildAssetBlob(
        mir::kAssetManifestTypeTag, mir::kAssetManifestContentVersion + 1, view->content);

    CHECK_FALSE(mir::AssetManifest::Parse(future).has_value());
}

TEST_CASE("AssetManifest rejects duplicate asset ids", "[assets][manifest]")
{
    const mir::AssetId id = mir::MakeAssetId("a/one.raw");

    // what an id collision looks like on disk: silently keeping the first would
    // make the second asset permanently unreachable
    const mir::AssetManifestSourceEntry entries[]{
        {{id, 1, 1}, "cooked/one.blob"},
        {{id, 1, 1}, "cooked/two.blob"},
    };

    const std::vector<std::byte> blob = mir::BuildAssetManifestBlob(entries);
    CHECK_FALSE(mir::AssetManifest::Parse(blob).has_value());
}

TEST_CASE("AssetManifest round trips logical paths and dependency edges", "[assets][manifest][graph]")
{
    const Graph graph;
    const std::vector<mir::AssetManifestSourceEntry> entries = graph.Entries();
    const std::optional<mir::AssetManifest> manifest = mir::AssetManifest::Parse(mir::BuildAssetManifestBlob(entries));
    REQUIRE(manifest.has_value());
    CHECK(manifest->Count() == 5);

    const mir::AssetManifestEntry *a = manifest->Find(graph.a);
    REQUIRE(a != nullptr);
    CHECK(manifest->PathOf(*a) == "a.blob");
    CHECK(manifest->LogicalPathOf(*a) == "g/a");
    const std::span<const mir::AssetId> aDeps = manifest->Dependencies(*a);
    REQUIRE(aDeps.size() == 2);
    CHECK(aDeps[0] == graph.b);
    CHECK(aDeps[1] == graph.c);

    // an entry with no edges or name reads back empty
    const mir::AssetManifestEntry *e = manifest->Find(graph.e);
    REQUIRE(e != nullptr);
    CHECK(manifest->Dependencies(*e).empty());
    CHECK(manifest->LogicalPathOf(*e).empty());
    CHECK(manifest->PathOf(*e) == "e.blob");
}

TEST_CASE("AssetManifest FindByPath resolves a logical path", "[assets][manifest][graph]")
{
    const Graph graph;
    const std::vector<mir::AssetManifestSourceEntry> entries = graph.Entries();
    const std::optional<mir::AssetManifest> manifest = mir::AssetManifest::Parse(mir::BuildAssetManifestBlob(entries));
    REQUIRE(manifest.has_value());

    const mir::AssetManifestEntry *found = manifest->FindByPath("g/a");
    REQUIRE(found != nullptr);
    CHECK(found->id == graph.a);

    CHECK(manifest->FindByPath("g/missing") == nullptr);
    // an empty name matches nothing
    CHECK(manifest->FindByPath("") == nullptr);
}

TEST_CASE("AssetManifest keeps working after a move", "[assets][manifest][graph]")
{
    const Graph graph;
    const std::vector<mir::AssetManifestSourceEntry> entries = graph.Entries();
    std::optional<mir::AssetManifest> parsed = mir::AssetManifest::Parse(mir::BuildAssetManifestBlob(entries));
    REQUIRE(parsed.has_value());

    // a short string lives inside its object, so a stale view breaks after the move
    const mir::AssetManifest moved = std::move(*parsed);
    const mir::AssetManifestEntry *found = moved.FindByPath("g/a");
    REQUIRE(found != nullptr);
    CHECK(moved.LogicalPathOf(*found) == "g/a");
}

TEST_CASE("AssetManifest rejects a duplicate logical path", "[assets][manifest][graph]")
{
    const mir::AssetManifestSourceEntry entries[]{
        { { mir::MakeAssetId("x/one"), 1, 1 }, "one.blob", "same/name", {} },
        { { mir::MakeAssetId("x/two"), 1, 1 }, "two.blob", "same/name", {} },
    };
    CHECK_FALSE(mir::AssetManifest::Parse(mir::BuildAssetManifestBlob(entries)).has_value());
}

TEST_CASE("AssetManifest rejects an edge to an asset that is not in the manifest", "[assets][manifest][graph]")
{
    const mir::AssetId missing = mir::MakeAssetId("x/missing");
    const std::vector<mir::AssetId> deps{ missing };
    const mir::AssetManifestSourceEntry entries[]{
        { { mir::MakeAssetId("x/one"), 1, 1 }, "one.blob", "", deps },
    };
    CHECK_FALSE(mir::AssetManifest::Parse(mir::BuildAssetManifestBlob(entries)).has_value());
}

TEST_CASE("AssetManifest rejects a dependency range outside the edge table", "[assets][manifest][graph]")
{
    const Graph graph;
    const std::vector<mir::AssetManifestSourceEntry> entries = graph.Entries();
    const std::vector<std::byte> good = mir::BuildAssetManifestBlob(entries);

    SECTION("count past the end")
    {
        const std::vector<std::byte> bad = Rewrapped(good, [](std::vector<std::byte> &content)
            {
                mir::AssetManifestEntry first;
                std::memcpy(&first, content.data() + kEntriesStart, sizeof(first));
                first.depCount = 1000;
                std::memcpy(content.data() + kEntriesStart, &first, sizeof(first));
            });
        CHECK_FALSE(mir::AssetManifest::Parse(bad).has_value());
    }

    SECTION("offset and count that overflow when added")
    {
        const std::vector<std::byte> bad = Rewrapped(good, [](std::vector<std::byte> &content)
            {
                mir::AssetManifestEntry first;
                std::memcpy(&first, content.data() + kEntriesStart, sizeof(first));
                first.depOffset = 0xFFFFFFFFu;
                first.depCount = 2;
                std::memcpy(content.data() + kEntriesStart, &first, sizeof(first));
            });
        CHECK_FALSE(mir::AssetManifest::Parse(bad).has_value());
    }

    SECTION("an edge count larger than the content")
    {
        const std::vector<std::byte> bad = Rewrapped(good, [](std::vector<std::byte> &content)
            {
                const uint64_t huge = ~uint64_t{ 0 };
                std::memcpy(content.data() + sizeof(uint64_t), &huge, sizeof(huge));
            });
        CHECK_FALSE(mir::AssetManifest::Parse(bad).has_value());
    }
}

TEST_CASE("AssetManifest rejects a logical path range outside the path table", "[assets][manifest][graph]")
{
    const Graph graph;
    const std::vector<mir::AssetManifestSourceEntry> entries = graph.Entries();
    const std::vector<std::byte> bad = Rewrapped(mir::BuildAssetManifestBlob(entries),
        [](std::vector<std::byte> &content)
        {
            mir::AssetManifestEntry first;
            std::memcpy(&first, content.data() + kEntriesStart, sizeof(first));
            first.logicalPathLength = 100000;
            std::memcpy(content.data() + kEntriesStart, &first, sizeof(first));
        });
    CHECK_FALSE(mir::AssetManifest::Parse(bad).has_value());
}

TEST_CASE("AssetManifest Closure lists the root first and each asset once", "[assets][manifest][graph]")
{
    const Graph graph;
    const std::vector<mir::AssetManifestSourceEntry> entries = graph.Entries();
    const std::optional<mir::AssetManifest> manifest = mir::AssetManifest::Parse(mir::BuildAssetManifestBlob(entries));
    REQUIRE(manifest.has_value());

    // D is reached twice and points back at A, so it appears once and the walk ends
    const std::vector<mir::AssetId> closure = manifest->Closure(graph.a);
    const std::vector<mir::AssetId> expected{ graph.a, graph.b, graph.c, graph.d, graph.e };
    CHECK(closure == expected);
}

TEST_CASE("AssetManifest Closure of an asset with no edges is itself", "[assets][manifest][graph]")
{
    const Graph graph;
    const std::vector<mir::AssetManifestSourceEntry> entries = graph.Entries();
    const std::optional<mir::AssetManifest> manifest = mir::AssetManifest::Parse(mir::BuildAssetManifestBlob(entries));
    REQUIRE(manifest.has_value());

    const std::vector<mir::AssetId> expected{ graph.e };
    CHECK(manifest->Closure(graph.e) == expected);
}

TEST_CASE("AssetManifest Closure of an unknown asset is empty", "[assets][manifest][graph]")
{
    const Graph graph;
    const std::vector<mir::AssetManifestSourceEntry> entries = graph.Entries();
    const std::optional<mir::AssetManifest> manifest = mir::AssetManifest::Parse(mir::BuildAssetManifestBlob(entries));
    REQUIRE(manifest.has_value());

    CHECK(manifest->Closure(mir::MakeAssetId("g/unknown")).empty());
}

TEST_CASE("AssetManifest Closure with tags keeps the root and skips unwanted types", "[assets][manifest][graph]")
{
    const Graph graph;
    const std::vector<mir::AssetManifestSourceEntry> entries = graph.Entries();
    const std::optional<mir::AssetManifest> manifest = mir::AssetManifest::Parse(mir::BuildAssetManifestBlob(entries));
    REQUIRE(manifest.has_value());

    SECTION("root is kept even when its own tag is not listed")
    {
        const uint32_t tags[]{ 2 };
        const std::vector<mir::AssetId> expected{ graph.a, graph.b };
        CHECK(manifest->Closure(graph.a, tags) == expected);
    }

    SECTION("a filtered asset hides what is only reachable through it")
    {
        // C is not wanted so E behind it is out, D is still reached through B
        const uint32_t tags[]{ 2, 4 };
        const std::vector<mir::AssetId> expected{ graph.a, graph.b, graph.d };
        CHECK(manifest->Closure(graph.a, tags) == expected);
    }

    SECTION("a tag no dependency has gives only the root")
    {
        const uint32_t tags[]{ 99 };
        const std::vector<mir::AssetId> expected{ graph.a };
        CHECK(manifest->Closure(graph.a, tags) == expected);
    }
}
