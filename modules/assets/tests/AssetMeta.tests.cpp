/**
 * @file AssetMeta.tests.cpp
 * @author sumin.park
 * @brief Tests for assets/AssetMeta: the .meta file, guids and the files scans skip.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <assets/AssetMeta.h>

#include <core/platform/Process.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unordered_set>

using namespace mir;
namespace fs = std::filesystem;

namespace
{
    // A scratch directory for one test
    struct Scratch
    {
        fs::path dir;

        Scratch()
        {
            static std::atomic<int> counter{ 0 };
            dir = fs::temp_directory_path() /
                  ("mir_asset_meta_" + std::to_string(CurrentProcessId()) + "_" + std::to_string(counter.fetch_add(1)));
            std::error_code ec;
            fs::remove_all(dir, ec);
            fs::create_directories(dir, ec);
        }

        ~Scratch()
        {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }

        void Write(const fs::path &path, const std::string &text) const { std::ofstream(path, std::ios::binary) << text; }
    };
}

TEST_CASE("A .meta file round trips its guid and importer state", "[assets][meta]")
{
    const Scratch scratch;
    const fs::path path = scratch.dir / "a.png.meta";

    AssetMeta meta;
    meta.guid = AssetId{ 0x0123456789ABCDEFull };
    meta.importer = R"({"next":3,"objects":{"mesh:name/Duck":"1"}})";
    REQUIRE(WriteAssetMeta(path, meta));

    const std::optional<AssetMeta> read = ReadAssetMeta(path);
    REQUIRE(read.has_value());
    CHECK(read->guid == meta.guid);
    CHECK(read->importer == meta.importer);
}

TEST_CASE("A .meta without importer state reads an empty importer", "[assets][meta]")
{
    const Scratch scratch;
    const fs::path path = scratch.dir / "a.meta";
    scratch.Write(path, R"({"version":1,"guid":"00000000000000ab"})");

    const std::optional<AssetMeta> read = ReadAssetMeta(path);
    REQUIRE(read.has_value());
    CHECK(read->guid == AssetId{ 0xAB });
    CHECK(read->importer == "{}");
}

TEST_CASE("A .meta that is missing reads as nothing, and one that is wrong is refused", "[assets][meta]")
{
    const Scratch scratch;
    const fs::path path = scratch.dir / "a.meta";
    CHECK_FALSE(ReadAssetMeta(path).has_value());

    for (const char *text : { "not json", "[]", R"({"guid":"00000000000000ab"})",
             R"({"version":2,"guid":"00000000000000ab"})", R"({"version":1})",
             R"({"version":1,"guid":"ab"})", R"({"version":1,"guid":"0000000000000000"})",
             R"({"version":1,"guid":"zzzzzzzzzzzzzzzz"})", R"({"version":1,"guid":"-0000000000000ab"})",
             R"({"version":1,"guid":12})", R"({"version":1,"guid":"00000000000000ab","importer":[]})" })
    {
        scratch.Write(path, text);
        INFO(text);
        CHECK_FALSE(ReadAssetMeta(path).has_value());
    }
}

TEST_CASE("A .meta is not written with a null guid or importer state that is not an object", "[assets][meta]")
{
    const Scratch scratch;
    const fs::path path = scratch.dir / "a.meta";

    AssetMeta meta;
    CHECK_FALSE(WriteAssetMeta(path, meta));

    meta.guid = AssetId{ 5 };
    meta.importer = "[1]";
    CHECK_FALSE(WriteAssetMeta(path, meta));
    meta.importer = "not json";
    CHECK_FALSE(WriteAssetMeta(path, meta));
    CHECK_FALSE(fs::exists(path));
}

TEST_CASE("The .meta of a source file sits beside it", "[assets][meta]")
{
    CHECK(AssetMetaPath("textures/white.png") == fs::path("textures/white.png.meta"));
    CHECK(AssetMetaPath("models/Duck.glb") == fs::path("models/Duck.glb.meta"));
}

TEST_CASE("Random guids are never null and never taken", "[assets][meta]")
{
    std::unordered_set<uint64_t> seen;
    for (int i = 0; i < 2000; ++i)
    {
        const AssetId guid = MakeRandomGuid(seen);
        REQUIRE_FALSE(guid.IsNull());
        REQUIRE(seen.insert(guid.value).second);
    }
}

TEST_CASE("A .meta moves with its source and a missing one is ignored", "[assets][meta]")
{
    const Scratch scratch;
    const fs::path from = scratch.dir / "old.png";
    const fs::path to = scratch.dir / "new.png";

    // nothing to move
    MoveAssetMeta(from, to);
    CHECK_FALSE(fs::exists(AssetMetaPath(to)));

    AssetMeta meta;
    meta.guid = AssetId{ 7 };
    REQUIRE(WriteAssetMeta(AssetMetaPath(from), meta));
    MoveAssetMeta(from, to);
    CHECK_FALSE(fs::exists(AssetMetaPath(from)));
    REQUIRE(ReadAssetMeta(AssetMetaPath(to)).has_value());
    CHECK(ReadAssetMeta(AssetMetaPath(to))->guid == AssetId{ 7 });
}

TEST_CASE("A .meta moved onto a leftover one replaces it", "[assets][meta]")
{
    const Scratch scratch;
    const fs::path from = scratch.dir / "old.png";
    const fs::path to = scratch.dir / "new.png";

    AssetMeta mine;
    mine.guid = AssetId{ 7 };
    AssetMeta leftover;
    leftover.guid = AssetId{ 9 };
    REQUIRE(WriteAssetMeta(AssetMetaPath(from), mine));
    REQUIRE(WriteAssetMeta(AssetMetaPath(to), leftover));

    MoveAssetMeta(from, to);
    CHECK(ReadAssetMeta(AssetMetaPath(to))->guid == AssetId{ 7 });
}

TEST_CASE("RemoveAssetMeta deletes the .meta and tolerates a missing one", "[assets][meta]")
{
    const Scratch scratch;
    const fs::path source = scratch.dir / "a.png";
    RemoveAssetMeta(source);

    AssetMeta meta;
    meta.guid = AssetId{ 7 };
    REQUIRE(WriteAssetMeta(AssetMetaPath(source), meta));
    RemoveAssetMeta(source);
    CHECK_FALSE(fs::exists(AssetMetaPath(source)));
}

TEST_CASE("Tooling files are the ones scans skip", "[assets][meta]")
{
    CHECK(IsAssetToolingFile("textures/white.png.meta"));
    CHECK(IsAssetToolingFile("models/Duck.glb.import.json"));
    CHECK(IsAssetToolingFile("manifest.blob.pending"));
    CHECK(IsAssetToolingFile("data/.gitkeep"));
    // case does not matter on Windows
    CHECK(IsAssetToolingFile("TEXTURES/WHITE.PNG.META"));

    CHECK_FALSE(IsAssetToolingFile("textures/white.png"));
    CHECK_FALSE(IsAssetToolingFile("particles/sparks.particle.json"));
    CHECK_FALSE(IsAssetToolingFile("scripts/spin.lua"));
    // only the whole .import.json suffix counts, not any .json
    CHECK_FALSE(IsAssetToolingFile("data/import.json"));
    CHECK_FALSE(IsAssetToolingFile("data/metadata"));
}
