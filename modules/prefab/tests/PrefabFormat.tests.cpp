/**
 * @file PrefabFormat.tests.cpp
 * @author Jackey Cen
 * @brief Tests for prefab/PrefabFormat.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <prefab/PrefabFormat.h>

#include <core/ecs/ComponentRegistry.h>

#include <serial/JsonFile.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using nlohmann::json;
    using mir::PrefabDiagnostic;
    using mir::PrefabDiagnosticSeverity;
    using mir::PrefabDocument;
    using mir::PrefabNode;
    using mir::PrefabNodeKind;
    using mir::PrefabParseResult;

    const char *kUuidA = "11111111-2222-4333-8444-555555555555";
    const char *kUuidB = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";

    const mir::ComponentOps &RefOps()
    {
        static const mir::RuntimeFieldDecl fields[] = {
            { "target", mir::FieldKind::EntityRef },
            { "speed", mir::FieldKind::Float },
        };
        return mir::ComponentRegistry::Instance().RegisterRuntime("PrefabFormatTestRef", fields);
    }

    json ValidPrefabJson()
    {
        json root;
        root["formatVersion"] = 1;
        root["prefabId"] = kUuidA;
        root["nextNodeId"] = "4";
        root["rootNode"] = "1";
        root["nodes"] = json::array({
            json{ { "id", "1" }, { "kind", "entity" } },
            json{ { "id", "2" }, { "kind", "entity" }, { "parent", "1" } },
            json{ { "id", "3" }, { "kind", "prefab" }, { "parent", "1" }, { "prefab", kUuidB } },
        });
        return root;
    }

    bool HasDiagnosticContaining(const std::vector<PrefabDiagnostic> &diagnostics,
        PrefabDiagnosticSeverity severity, std::string_view needle)
    {
        for (const PrefabDiagnostic &diagnostic : diagnostics)
        {
            if (diagnostic.mSeverity == severity && diagnostic.mMessage.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }

    bool HasErrorContaining(const std::vector<PrefabDiagnostic> &diagnostics, std::string_view needle)
    {
        return HasDiagnosticContaining(diagnostics, PrefabDiagnosticSeverity::Error, needle);
    }

    std::size_t CountErrors(const std::vector<PrefabDiagnostic> &diagnostics)
    {
        std::size_t count = 0;
        for (const PrefabDiagnostic &diagnostic : diagnostics)
        {
            if (diagnostic.mSeverity == PrefabDiagnosticSeverity::Error)
                ++count;
        }
        return count;
    }

    // catch_discover_tests runs each TEST_CASE in its own process, so the temp
    // root carries a per-process token and a counter to survive parallel ctest.
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
                    ("mir_prefab_format_test_" + ProcessToken() + "_" + std::to_string(counter.fetch_add(1)));
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
}

TEST_CASE("NewPrefabDocument starts with a root entity node", "[prefab]")
{
    const PrefabDocument doc = mir::NewPrefabDocument();

    REQUIRE_FALSE(doc.mId.IsNull());
    REQUIRE(doc.mRootNode == 1);
    REQUIRE(doc.mNextNodeId == 2);
    REQUIRE(doc.mNodes.size() == 1);
    REQUIRE(doc.mNodes[0].mKind == PrefabNodeKind::Entity);
    REQUIRE(doc.mNodes[0].mParent == mir::kNullPrefabNodeId);
    REQUIRE(mir::ValidatePrefab(doc).empty());
}

TEST_CASE("AllocateNodeId never reissues ids", "[prefab]")
{
    PrefabDocument doc = mir::NewPrefabDocument();
    const std::optional<mir::PrefabNodeId> first = mir::AllocateNodeId(doc);
    const std::optional<mir::PrefabNodeId> second = mir::AllocateNodeId(doc);
    REQUIRE(first == 2);
    REQUIRE(second == 3);

    PrefabNode child;
    child.mId = *first;
    child.mParent = doc.mRootNode;
    doc.mNodes.push_back(child);
    doc.mNodes.erase(doc.mNodes.begin() + 1);

    const std::optional<mir::PrefabNodeId> third = mir::AllocateNodeId(doc);
    REQUIRE(third == 4);
}

TEST_CASE("AllocateNodeId fails at the top of the id space", "[prefab]")
{
    PrefabDocument doc = mir::NewPrefabDocument();
    doc.mNextNodeId = std::numeric_limits<mir::PrefabNodeId>::max();

    REQUIRE(mir::AllocateNodeId(doc) == std::nullopt);
    REQUIRE(doc.mNextNodeId == std::numeric_limits<mir::PrefabNodeId>::max());
}

TEST_CASE("Write-read-write is text-identical", "[prefab]")
{
    RefOps();
    TempDir dir;

    PrefabDocument doc = mir::NewPrefabDocument();

    PrefabNode child;
    child.mId = *mir::AllocateNodeId(doc);
    child.mParent = doc.mRootNode;
    child.mComponents.push_back({ "PrefabFormatTestRef", json{ { "target", nullptr }, { "speed", 1.5 } } });
    child.mComponents.push_back({ "PrefabFormatTestUnknown", json{ { "x", json::array({ 1, 2, 3 }) } } });
    doc.mNodes.push_back(child);

    PrefabNode nested;
    nested.mId = *mir::AllocateNodeId(doc);
    nested.mKind = PrefabNodeKind::Prefab;
    nested.mParent = doc.mRootNode;
    nested.mPrefab = *mir::ParsePrefabId(kUuidB);

    mir::PrefabPatch patch;
    patch.mTarget = { nested.mId, 9 };
    patch.mFields.push_back({ "PrefabFormatTestRef", "target", nullptr });
    patch.mFields.push_back({ "PrefabFormatTestRef", "speed", 2.5 });
    patch.mAddedComponents.push_back({ "PrefabFormatTestRef", json{ { "target", json::array({ "2" }) } } });
    patch.mRemovedComponents.push_back("Transform");
    nested.mPatches.push_back(std::move(patch));
    doc.mNodes.push_back(nested);

    const std::filesystem::path firstPath = dir.mPath / "first.prefab";
    const std::filesystem::path secondPath = dir.mPath / "second.prefab";

    std::vector<PrefabDiagnostic> diagnostics;
    REQUIRE(mir::WritePrefabFile(firstPath, doc, &diagnostics));

    const PrefabParseResult parsed = mir::ReadPrefabFile(firstPath);
    REQUIRE(parsed.mDocument.has_value());
    REQUIRE_FALSE(mir::HasPrefabErrors(parsed.mDiagnostics));

    PrefabDocument loaded = *parsed.mDocument;
    REQUIRE(loaded.mId == doc.mId);
    REQUIRE(loaded.mNextNodeId == doc.mNextNodeId);
    REQUIRE(loaded.mRootNode == doc.mRootNode);
    REQUIRE(loaded.mNodes.size() == doc.mNodes.size());
    REQUIRE(loaded.mNodes[1].mComponents.size() == 2);
    REQUIRE(loaded.mNodes[1].mComponents[1].mType == "PrefabFormatTestUnknown");
    REQUIRE(loaded.mNodes[1].mComponents[1].mFields == child.mComponents[1].mFields);
    REQUIRE(loaded.mNodes[2].mPrefab == nested.mPrefab);
    REQUIRE(loaded.mNodes[2].mPatches.size() == 1);
    REQUIRE(loaded.mNodes[2].mPatches[0].mTarget == mir::PrefabNodeAddress({ nested.mId, 9 }));
    REQUIRE(loaded.mNodes[2].mPatches[0].mFields.size() == 2);
    REQUIRE(loaded.mNodes[2].mPatches[0].mAddedComponents.size() == 1);
    REQUIRE(loaded.mNodes[2].mPatches[0].mRemovedComponents == std::vector<std::string>{ "Transform" });

    REQUIRE(mir::WritePrefabFile(secondPath, loaded));
    REQUIRE(ReadText(firstPath) == ReadText(secondPath));

    const std::optional<mir::PrefabNodeId> afterLoad = mir::AllocateNodeId(loaded);
    REQUIRE(afterLoad == doc.mNextNodeId);
}

TEST_CASE("Ids written as JSON numbers are rejected with a string hint", "[prefab]")
{
    SECTION("nextNodeId")
    {
        json root = ValidPrefabJson();
        root["nextNodeId"] = 4;
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE_FALSE(result.mDocument.has_value());
        REQUIRE(HasErrorContaining(result.mDiagnostics, "must be written as a string"));
    }
    SECTION("node id")
    {
        json root = ValidPrefabJson();
        root["nodes"][0]["id"] = 1;
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE_FALSE(result.mDocument.has_value());
        REQUIRE(HasErrorContaining(result.mDiagnostics, "must be written as a string"));
    }
    SECTION("parent")
    {
        json root = ValidPrefabJson();
        root["nodes"][1]["parent"] = 1;
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE_FALSE(result.mDocument.has_value());
        REQUIRE(HasErrorContaining(result.mDiagnostics, "must be written as a string"));
    }
}

TEST_CASE("Node id strings are strict decimals", "[prefab]")
{
    const char *bad[] = { "01", "0", " 1", "1 ", "+1", "-1", "1.0", "18446744073709551616", "abc" };
    for (const char *text : bad)
    {
        json root = ValidPrefabJson();
        root["nodes"][0]["id"] = text;
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE_FALSE(result.mDocument.has_value());
        REQUIRE(HasErrorContaining(result.mDiagnostics, "is not a valid node id"));
    }
}

TEST_CASE("formatVersion is enforced", "[prefab]")
{
    SECTION("missing")
    {
        json root = ValidPrefabJson();
        root.erase("formatVersion");
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE(HasErrorContaining(result.mDiagnostics, "missing \"formatVersion\""));
    }
    SECTION("newer")
    {
        json root = ValidPrefabJson();
        root["formatVersion"] = 2;
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE(HasErrorContaining(result.mDiagnostics, "newer prefab format"));
    }
    SECTION("large unsigned")
    {
        json root = ValidPrefabJson();
        root["formatVersion"] = 4294967297ULL;
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE(HasErrorContaining(result.mDiagnostics, "newer prefab format"));
    }
    SECTION("older")
    {
        json root = ValidPrefabJson();
        root["formatVersion"] = 0;
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE(HasErrorContaining(result.mDiagnostics, "unsupported prefab format version"));
    }
    SECTION("not a number")
    {
        json root = ValidPrefabJson();
        root["formatVersion"] = "1";
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE(HasErrorContaining(result.mDiagnostics, "must be the number"));
    }
}

TEST_CASE("prefabId must be a valid non-nil UUID", "[prefab]")
{
    SECTION("garbage")
    {
        json root = ValidPrefabJson();
        root["prefabId"] = "not-a-uuid";
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE(HasErrorContaining(result.mDiagnostics, "not a valid non-nil UUID"));
    }
    SECTION("nil")
    {
        json root = ValidPrefabJson();
        root["prefabId"] = "00000000-0000-0000-0000-000000000000";
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE(HasErrorContaining(result.mDiagnostics, "not a valid non-nil UUID"));
    }
    SECTION("not a string")
    {
        json root = ValidPrefabJson();
        root["prefabId"] = 42;
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE(HasErrorContaining(result.mDiagnostics, "must be a non-nil UUID string"));
    }
}

TEST_CASE("Graph validation names the broken nodes", "[prefab]")
{
    SECTION("duplicate node id")
    {
        json root = ValidPrefabJson();
        root["nodes"].push_back(json{ { "id", "2" }, { "kind", "entity" }, { "parent", "1" } });
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE(HasErrorContaining(result.mDiagnostics, "duplicate node id 2"));
    }
    SECTION("node id past the high-water mark")
    {
        json root = ValidPrefabJson();
        root["nodes"][1]["id"] = "4";
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE(HasErrorContaining(result.mDiagnostics, "node 4 is at or above nextNodeId"));
    }
    SECTION("root names no node")
    {
        json root = ValidPrefabJson();
        root["rootNode"] = "9";
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE(HasErrorContaining(result.mDiagnostics, "rootNode 9 does not name a node"));
    }
    SECTION("root is a nested reference")
    {
        json root = ValidPrefabJson();
        root["rootNode"] = "3";
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE(HasErrorContaining(result.mDiagnostics, "root node 3 must be an entity node"));
    }
    SECTION("root has a parent")
    {
        json root = ValidPrefabJson();
        root["nodes"][0]["parent"] = "2";
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE(HasErrorContaining(result.mDiagnostics, "root node 1 must not have a parent"));
    }
    SECTION("non-root without a parent")
    {
        json root = ValidPrefabJson();
        root["nodes"][1].erase("parent");
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE(HasErrorContaining(result.mDiagnostics, "node 2 has no parent"));
    }
    SECTION("parent names no node")
    {
        json root = ValidPrefabJson();
        root["nodes"][1]["parent"] = "9";
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE(HasErrorContaining(result.mDiagnostics, "node 2 parent 9 does not name a node"));
    }
    SECTION("parent is a nested reference")
    {
        json root = ValidPrefabJson();
        root["nodes"][1]["parent"] = "3";
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE(HasErrorContaining(result.mDiagnostics, "only entity nodes have children"));
    }
}

TEST_CASE("Cycles and disconnected nodes are diagnosed", "[prefab]")
{
    json root = ValidPrefabJson();
    root["nextNodeId"] = "5";
    root["nodes"][1]["parent"] = "3";
    root["nodes"][2] = json{ { "id", "3" }, { "kind", "entity" }, { "parent", "2" } };
    root["nodes"].push_back(json{ { "id", "4" }, { "kind", "entity" }, { "parent", "2" } });

    const PrefabParseResult result = mir::ParsePrefab(root);
    REQUIRE_FALSE(result.mDocument.has_value());
    REQUIRE(HasErrorContaining(result.mDiagnostics, "node 2 is in a parent cycle"));
    REQUIRE(HasErrorContaining(result.mDiagnostics, "node 3 is in a parent cycle"));
    REQUIRE(HasErrorContaining(result.mDiagnostics, "node 4 is unreachable from the root"));
}

TEST_CASE("Nested reference targets are validated", "[prefab]")
{
    SECTION("invalid uuid")
    {
        json root = ValidPrefabJson();
        root["nodes"][2]["prefab"] = "garbage";
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE(HasErrorContaining(result.mDiagnostics, "not a valid non-nil UUID"));
    }
    SECTION("self reference")
    {
        json root = ValidPrefabJson();
        root["nodes"][2]["prefab"] = kUuidA;
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE(HasErrorContaining(result.mDiagnostics, "references this prefab itself"));
    }
}

TEST_CASE("Patch targets are addresses inside the referenced prefab", "[prefab]")
{
    auto withTarget = [](json address)
        {
            json root = ValidPrefabJson();
            root["nodes"][2]["patches"] = json::array({ json{ { "target", std::move(address) } } });
            return mir::ParsePrefab(root);
        };

    REQUIRE(withTarget(json::array({ "5" })).mDocument.has_value());
    REQUIRE(withTarget(json::array({ "7", "5" })).mDocument.has_value());

    REQUIRE(HasErrorContaining(withTarget(json::array()).mDiagnostics, "empty target address"));
    REQUIRE(HasErrorContaining(withTarget(json::array({ 5 })).mDiagnostics, "must be written as a string"));
    REQUIRE(HasErrorContaining(withTarget(json::array({ "0" })).mDiagnostics, "is not a valid node id"));
}

TEST_CASE("Duplicate component entries are diagnosed", "[prefab]")
{
    SECTION("on an entity node")
    {
        json root = ValidPrefabJson();
        root["nodes"][1]["components"] = json::array({
            json{ { "type", "A" }, { "fields", json::object() } },
            json{ { "type", "A" }, { "fields", json::object() } },
        });
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE(HasErrorContaining(result.mDiagnostics, "duplicate component type \"A\""));
    }
    SECTION("in addedComponents")
    {
        json root = ValidPrefabJson();
        root["nodes"][2]["patches"] = json::array({ json{
            { "target", json::array({ "3", "5" }) },
            { "addedComponents", json::array({
                json{ { "type", "A" }, { "fields", json::object() } },
                json{ { "type", "A" }, { "fields", json::object() } },
            }) },
        } });
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE(HasErrorContaining(result.mDiagnostics, "adds component type \"A\" twice"));
    }
    SECTION("in removedComponents")
    {
        json root = ValidPrefabJson();
        root["nodes"][2]["patches"] = json::array({ json{
            { "target", json::array({ "3", "5" }) },
            { "removedComponents", json::array({ "A", "A" }) },
        } });
        const PrefabParseResult result = mir::ParsePrefab(root);
        REQUIRE(HasErrorContaining(result.mDiagnostics, "removes component type \"A\" twice"));
    }
}

TEST_CASE("EntityRef fields accept null or document addresses", "[prefab]")
{
    RefOps();
    auto withTarget = [](json value)
        {
            json root = ValidPrefabJson();
            root["nodes"][1]["components"] = json::array({
                json{ { "type", "PrefabFormatTestRef" }, { "fields", json{ { "target", std::move(value) } } } },
            });
            return mir::ParsePrefab(root);
        };

    REQUIRE(withTarget(nullptr).mDocument.has_value());
    REQUIRE(withTarget(json::array({ "2" })).mDocument.has_value());
    REQUIRE(withTarget(json::array({ "3", "9" })).mDocument.has_value());

    REQUIRE(HasErrorContaining(withTarget(5).mDiagnostics, "never persisted"));
    REQUIRE(HasErrorContaining(withTarget(json{ { "entity", 5 } }).mDiagnostics, "never persisted"));
    REQUIRE(HasErrorContaining(withTarget(json::array({ "2", "9" })).mDiagnostics, "is not a nested reference"));
    REQUIRE(HasErrorContaining(withTarget(json::array({ "9" })).mDiagnostics, "not in this document"));
    REQUIRE(HasErrorContaining(withTarget(json::array()).mDiagnostics, "empty node address"));
    REQUIRE(HasErrorContaining(withTarget(json::array({ 2 })).mDiagnostics, "must be written as a string"));
}

TEST_CASE("EntityRef checks apply to patch layers", "[prefab]")
{
    RefOps();
    auto withPatch = [](json patch)
        {
            json root = ValidPrefabJson();
            patch["target"] = json::array({ "3", "5" });
            root["nodes"][2]["patches"] = json::array({ std::move(patch) });
            return mir::ParsePrefab(root);
        };

    SECTION("added component with a runtime handle")
    {
        const PrefabParseResult result = withPatch(json{
            { "addedComponents", json::array({
                json{ { "type", "PrefabFormatTestRef" }, { "fields", json{ { "target", 7 } } } },
            }) },
        });
        REQUIRE(HasErrorContaining(result.mDiagnostics, "never persisted"));
    }
    SECTION("field override with a runtime handle")
    {
        const PrefabParseResult result = withPatch(json{
            { "fields", json::array({
                json{ { "component", "PrefabFormatTestRef" }, { "field", "target" }, { "value", json{ { "entity", 1 } } } },
            }) },
        });
        REQUIRE(HasErrorContaining(result.mDiagnostics, "never persisted"));
    }
    SECTION("non-EntityRef override is not shape-checked")
    {
        const PrefabParseResult result = withPatch(json{
            { "fields", json::array({
                json{ { "component", "PrefabFormatTestRef" }, { "field", "speed" }, { "value", 7 } },
            }) },
        });
        REQUIRE(result.mDocument.has_value());
    }
    SECTION("override on an unregistered component is not shape-checked")
    {
        const PrefabParseResult result = withPatch(json{
            { "fields", json::array({
                json{ { "component", "PrefabFormatTestUnknown" }, { "field", "target" }, { "value", 7 } },
            }) },
        });
        REQUIRE(result.mDocument.has_value());
    }
}

TEST_CASE("Unknown component types warn but survive", "[prefab]")
{
    json root = ValidPrefabJson();
    root["nodes"][1]["components"] = json::array({
        json{ { "type", "PrefabFormatTestUnknown" }, { "fields", json{ { "anything", json::array({ 1, 2, 3 }) } } } },
    });

    const PrefabParseResult result = mir::ParsePrefab(root);
    REQUIRE(result.mDocument.has_value());
    REQUIRE(HasDiagnosticContaining(result.mDiagnostics, PrefabDiagnosticSeverity::Warning, "not registered"));
    REQUIRE(result.mDocument->mNodes[1].mComponents[0].mFields.at("anything") == json::array({ 1, 2, 3 }));
}

TEST_CASE("Validation collects every error", "[prefab]")
{
    json root = ValidPrefabJson();
    root["rootNode"] = "9";
    root["nodes"][1].erase("parent");
    root["nodes"][2]["prefab"] = kUuidA;

    const PrefabParseResult result = mir::ParsePrefab(root);
    REQUIRE_FALSE(result.mDocument.has_value());
    REQUIRE(CountErrors(result.mDiagnostics) >= 3);
    REQUIRE(HasErrorContaining(result.mDiagnostics, "rootNode 9 does not name a node"));
    REQUIRE(HasErrorContaining(result.mDiagnostics, "node 2 has no parent"));
    REQUIRE(HasErrorContaining(result.mDiagnostics, "references this prefab itself"));
}

TEST_CASE("WritePrefabFile refuses an invalid document", "[prefab]")
{
    TempDir dir;
    PrefabDocument doc = mir::NewPrefabDocument();
    doc.mRootNode = 99;

    std::vector<PrefabDiagnostic> diagnostics;
    const std::filesystem::path path = dir.mPath / "invalid.prefab";
    REQUIRE_FALSE(mir::WritePrefabFile(path, doc, &diagnostics));
    REQUIRE_FALSE(std::filesystem::exists(path));
    REQUIRE_FALSE(diagnostics.empty());
}

TEST_CASE("Diagnostics carry the file path", "[prefab]")
{
    TempDir dir;

    SECTION("malformed JSON")
    {
        const std::filesystem::path path = dir.mPath / "broken.prefab";
        {
            std::ofstream file(path);
            file << "{ not json";
        }
        const PrefabParseResult result = mir::ReadPrefabFile(path);
        REQUIRE_FALSE(result.mDocument.has_value());
        REQUIRE(result.mDiagnostics.size() == 1);
        REQUIRE(result.mDiagnostics[0].mPath == path.string());
        REQUIRE(HasErrorContaining(result.mDiagnostics, "could not be read as JSON"));
    }
    SECTION("invalid document")
    {
        json root = ValidPrefabJson();
        root["rootNode"] = "9";
        const std::filesystem::path path = dir.mPath / "semantic.prefab";
        mir::WriteJsonFile(path, root);

        const PrefabParseResult result = mir::ReadPrefabFile(path);
        REQUIRE_FALSE(result.mDocument.has_value());
        REQUIRE_FALSE(result.mDiagnostics.empty());
        REQUIRE(result.mDiagnostics[0].mPath == path.string());
    }
}
