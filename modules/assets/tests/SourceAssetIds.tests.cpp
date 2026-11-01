/**
 * @file SourceAssetIds.tests.cpp
 * @author sumin.park
 * @brief Tests for assets/SourceAssetIds: guids of the source files under the asset roots.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <assets/SourceAssetIds.h>

#include <core/platform/Process.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace mir;
namespace fs = std::filesystem;

namespace
{
    // Two roots, the game's and the engine's, under one scratch directory
    struct Roots
    {
        fs::path dir;

        Roots()
        {
            static std::atomic<int> counter{ 0 };
            dir = fs::temp_directory_path() /
                ("mir_source_ids_" + std::to_string(CurrentProcessId()) + "_" + std::to_string(counter.fetch_add(1)));
            std::error_code ec;
            fs::remove_all(dir, ec);
            fs::create_directories(dir / "game", ec);
            fs::create_directories(dir / "engine", ec);
        }

        ~Roots()
        {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }

        std::vector<AssetSourceRoot> List() const
        {
            return { { dir / "game", "games/Game/assets" },{ dir / "engine", "assets" } };
        }

        fs::path File(const std::string &relative, const std::string &text = "x") const
        {
            const fs::path path = dir / relative;
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << text;
            return path;
        }

        void WriteMeta(const fs::path &source, uint64_t guid) const
        {
            AssetMeta meta;
            meta.guid = AssetId{ guid };
            REQUIRE(WriteAssetMeta(AssetMetaPath(source), meta));
        }
    };
}

TEST_CASE("A file with no .meta gets one with a new guid that stays", "[assets][sourceids]")
{
    const Roots roots;
    const fs::path a = roots.File("game/a.png");
    const fs::path b = roots.File("game/b.png");

    SourceAssetIds ids(roots.List());
    ids.Scan();
    const AssetId first = ids.Meta(a).guid;
    const AssetId second = ids.Meta(b).guid;
    CHECK_FALSE(first.IsNull());
    CHECK(first != second);
    CHECK(fs::exists(AssetMetaPath(a)));
    CHECK(ids.Meta(a).guid == first);

    // a later run reads the same guid back
    SourceAssetIds again(roots.List());
    again.Scan();
    CHECK(again.Meta(a).guid == first);
    CHECK(again.Meta(b).guid == second);
}

TEST_CASE("Scan reads the guids that are already written", "[assets][sourceids]")
{
    const Roots roots;
    const fs::path a = roots.File("game/a.png");
    roots.WriteMeta(a, 0xAAAA);

    SourceAssetIds ids(roots.List());
    ids.Scan();
    CHECK(ids.Meta(a).guid == AssetId{ 0xAAAA });
}

TEST_CASE("Two files with one guid fail the scan and the error names both", "[assets][sourceids]")
{
    const Roots roots;
    roots.WriteMeta(roots.File("game/a.png"), 0xBBBB);
    roots.WriteMeta(roots.File("engine/copy.png"), 0xBBBB);

    SourceAssetIds ids(roots.List());
    try
    {
        ids.Scan();
        FAIL("a duplicate guid was accepted");
    }
    catch (const std::runtime_error &error)
    {
        const std::string message = error.what();
        CHECK(message.find("a.png") != std::string::npos);
        CHECK(message.find("copy.png") != std::string::npos);
        CHECK(message.find("000000000000bbbb") != std::string::npos);
    }
}

TEST_CASE("A copied .meta is caught even without a scan", "[assets][sourceids]")
{
    const Roots roots;
    const fs::path a = roots.File("game/a.png");
    const fs::path copy = roots.File("game/copy.png");
    roots.WriteMeta(a, 0xCCCC);
    roots.WriteMeta(copy, 0xCCCC);

    SourceAssetIds ids(roots.List());
    CHECK(ids.Meta(a).guid == AssetId{ 0xCCCC });
    CHECK_THROWS_AS(ids.Meta(copy), std::runtime_error);
}

TEST_CASE("A .meta with no source is ignored and one that is invalid fails", "[assets][sourceids]")
{
    const Roots roots;
    const fs::path gone = roots.dir / "game" / "gone.png";
    roots.WriteMeta(gone, 0xDDDD);

    SourceAssetIds ignored(roots.List());
    CHECK_NOTHROW(ignored.Scan());

    const fs::path broken = roots.File("game/broken.png");
    roots.File("game/broken.png.meta", "not json");
    SourceAssetIds invalid(roots.List());
    CHECK_THROWS_AS(invalid.Scan(), std::runtime_error);
    (void)broken;
}

TEST_CASE("IdAt finds the file a logical path names, in either root", "[assets][sourceids]")
{
    const Roots roots;
    const fs::path game = roots.File("game/particles/white.png");
    const fs::path engine = roots.File("engine/ui/panel.png");

    SourceAssetIds ids(roots.List());
    ids.Scan();
    const AssetId gameId = ids.IdAt("games/Game/assets/particles/white.png");
    const AssetId engineId = ids.IdAt("assets/ui/panel.png");
    CHECK(gameId == ids.Meta(game).guid);
    CHECK(engineId == ids.Meta(engine).guid);
    CHECK(gameId != engineId);
}

TEST_CASE("IdAt is null for a path no root holds", "[assets][sourceids]")
{
    const Roots roots;
    roots.File("game/a.png");

    SourceAssetIds ids(roots.List());
    ids.Scan();
    CHECK(ids.IdAt("games/Game/assets/missing.png").IsNull());
    CHECK(ids.IdAt("other/a.png").IsNull());
    // a root name that only starts the same is not that root
    CHECK(ids.IdAt("games/Game/assetsX/a.png").IsNull());
    CHECK(ids.IdAt("games/Game/assets").IsNull());
    CHECK(ids.IdAt("").IsNull());
}

TEST_CASE("Importer state saved on a file is read back next time", "[assets][sourceids]")
{
    const Roots roots;
    const fs::path model = roots.File("game/duck.glb");

    SourceAssetIds ids(roots.List());
    ids.Scan();
    ids.Meta(model).importer = R"({"next":2})";
    ids.Save(model);
    const AssetId guid = ids.Meta(model).guid;

    SourceAssetIds again(roots.List());
    again.Scan();
    CHECK(again.Meta(model).importer == R"({"next":2})");
    CHECK(again.Meta(model).guid == guid);
}

TEST_CASE("A long-lived SourceAssetIds follows a file renamed with its .meta", "[assets][sourceids]")
{
    const Roots roots;
    const fs::path a = roots.File("game/particles/a.particle.json");
    SourceAssetIds ids(roots.List());
    const AssetId before = ids.IdAt("games/Game/assets/particles/a.particle.json");
    REQUIRE_FALSE(before.IsNull());

    const fs::path b = roots.dir / "game/particles/b.particle.json";
    fs::rename(a, b);
    MoveAssetMeta(a, b);
    AssetId after;
    REQUIRE_NOTHROW(after = ids.IdAt("games/Game/assets/particles/b.particle.json"));
    CHECK(after == before);
}

TEST_CASE("A guid is still a duplicate while both files exist", "[assets][sourceids]")
{
    const Roots roots;
    const fs::path a = roots.File("game/a.png");
    const fs::path copy = roots.File("game/copy.png");
    SourceAssetIds ids(roots.List());
    const AssetId guid = ids.Meta(a).guid;
    roots.WriteMeta(copy, guid.value);
    CHECK_THROWS_AS(ids.Meta(copy), std::runtime_error);
}
