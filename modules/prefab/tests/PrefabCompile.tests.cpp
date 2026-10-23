/**
 * @file PrefabCompile.tests.cpp
 * @author Jackey Cen
 * @brief Tests for prefab/CompiledPrefab.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <prefab/CompiledPrefab.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/components/Transform.h>
#include <core/math/WorldReal.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using nlohmann::json;
    using mir::CompiledPrefab;
    using mir::CompiledPrefabCache;
    using mir::CompiledPrefabComponent;
    using mir::CompiledPrefabNode;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::Entity;
    using mir::FieldKind;
    using mir::PrefabCompileResult;
    using mir::PrefabComponentInstance;
    using mir::PrefabDiagnostic;
    using mir::PrefabDiagnosticSeverity;
    using mir::PrefabDocument;
    using mir::PrefabDocumentLookup;
    using mir::PrefabId;
    using mir::PrefabNode;
    using mir::PrefabNodeAddress;
    using mir::PrefabNodeId;
    using mir::PrefabNodeKind;
    using mir::PrefabParseResult;
    using mir::RuntimeFieldDecl;
    using mir::WorldVec3;

    const char *kUuidA = "11111111-2222-4333-8444-555555555555";
    const char *kUuidB = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";

    PrefabDocumentLookup NoLookup()
    {
        return [](PrefabId) -> const PrefabDocument *
        {
            return nullptr;
        };
    }

    PrefabDocumentLookup LookupOf(const std::map<PrefabId, PrefabDocument> &documents)
    {
        return [&documents](PrefabId id) -> const PrefabDocument *
        {
            const auto it = documents.find(id);
            return it == documents.end() ? nullptr : &it->second;
        };
    }

    const ComponentOps &StatsOps()
    {
        static const RuntimeFieldDecl fields[] = {
            { "target", FieldKind::EntityRef },
            { "speed", FieldKind::Float },
            { "count", FieldKind::Int },
            { "flag", FieldKind::Bool },
            { "offset", FieldKind::Vec3 },
            { "position", FieldKind::WorldPosition },
            { "label", FieldKind::String },
        };
        return ComponentRegistry::Instance().RegisterRuntime("PrefabCompileTestStats", fields);
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

    bool HasWarningContaining(const std::vector<PrefabDiagnostic> &diagnostics, std::string_view needle)
    {
        return HasDiagnosticContaining(diagnostics, PrefabDiagnosticSeverity::Warning, needle);
    }

    PrefabDocument ParseDocument(const json &root)
    {
        const PrefabParseResult parsed = mir::ParsePrefab(root);
        REQUIRE_FALSE(mir::HasPrefabErrors(parsed.mDiagnostics));
        REQUIRE(parsed.mDocument.has_value());
        return *parsed.mDocument;
    }

    json CompileDocumentJson()
    {
        json root;
        root["formatVersion"] = 1;
        root["prefabId"] = kUuidA;
        root["nextNodeId"] = "5";
        root["rootNode"] = "1";
        root["nodes"] = json::array({
            json{
                { "id", "1" },
                { "kind", "entity" },
                { "components", json::array({
                    json{ { "type", "Transform" }, { "fields", json{
                        { "position", json::array({ 1.5, -2.25, 3.0 }) },
                        { "rotation", json::array({ 0.0, 0.0, 0.0, 1.0 }) },
                        { "scale", json::array({ 2.0, 2.0, 2.0 }) },
                    } } },
                    json{ { "type", "PrefabCompileTestStats" }, { "fields", json{
                        { "target", json::array({ "3" }) },
                        { "speed", 2.5 },
                        { "count", 7 },
                        { "flag", true },
                        { "offset", json::array({ 1.0, 2.0, 3.0 }) },
                        { "position", json::array({ 10.0, 20.0, 30.0 }) },
                        { "label", "hero" },
                    } } },
                }) },
            },
            json{
                { "id", "2" },
                { "kind", "entity" },
                { "parent", "1" },
                { "components", json::array({
                    json{ { "type", "PrefabCompileTestStats" }, { "fields", json{ { "speed", 1.25 } } } },
                }) },
            },
            json{
                { "id", "3" },
                { "kind", "entity" },
                { "parent", "1" },
            },
            json{
                { "id", "4" },
                { "kind", "entity" },
                { "parent", "2" },
            },
        });
        return root;
    }

    template <typename T>
    T ReadField(const CompiledPrefabComponent &component, std::string_view name)
    {
        const mir::FieldDesc *field = component.mOps->FindField(name);
        REQUIRE(field != nullptr);
        T value{};
        field->Read(component.mBytes.data(), &value);
        return value;
    }

    std::string ReadString(const CompiledPrefabComponent &component, std::string_view name)
    {
        const mir::FieldDesc *field = component.mOps->FindField(name);
        REQUIRE(field != nullptr);
        char buffer[mir::kFieldStringCapacity]{};
        field->Read(component.mBytes.data(), buffer);
        return std::string(buffer);
    }

    const CompiledPrefabComponent &ComponentNamed(const CompiledPrefabNode &node, std::string_view name)
    {
        for (const CompiledPrefabComponent &component : node.mComponents)
        {
            if (component.mOps->mType.name == name)
                return component;
        }
        FAIL("component missing");
        return node.mComponents.front();
    }

    const CompiledPrefabComponent &RootTransform(const CompiledPrefab &prefab)
    {
        REQUIRE(prefab.Nodes().size() == 1);
        return ComponentNamed(prefab.Nodes()[0], "Transform");
    }
}

TEST_CASE("CompilePrefab decodes authored fields over registered defaults", "[prefab]")
{
    mir::RegisterCoreComponents();
    StatsOps();

    const PrefabDocument doc = ParseDocument(CompileDocumentJson());
    const PrefabCompileResult result = mir::CompilePrefab(doc, NoLookup());

    REQUIRE(result.mPrefab != nullptr);
    CHECK_FALSE(mir::HasPrefabErrors(result.mDiagnostics));
    const CompiledPrefab &prefab = *result.mPrefab;
    CHECK(prefab.Id() == doc.mId);
    CHECK(prefab.Revision() == 0);
    REQUIRE(prefab.Nodes().size() == 4);

    const std::array<PrefabNodeId, 4> expectedIds{ 1, 2, 4, 3 };
    for (std::size_t i = 0; i < expectedIds.size(); ++i)
        CHECK(prefab.Nodes()[i].mAddress == PrefabNodeAddress{ expectedIds[i] });

    CHECK_FALSE(prefab.Nodes()[0].mParent.has_value());
    CHECK(prefab.Nodes()[1].mParent == 0);
    CHECK(prefab.Nodes()[2].mParent == 1);
    CHECK(prefab.Nodes()[3].mParent == 0);

    const CompiledPrefabNode &root = prefab.Nodes()[0];
    REQUIRE(root.mComponents.size() == 2);
    CHECK(root.mComponents[0].mOps->mType.name == "Transform");
    CHECK(root.mComponents[1].mOps->mType.name == "PrefabCompileTestStats");

    const CompiledPrefabComponent &transform = root.mComponents[0];
    CHECK(ReadField<WorldVec3>(transform, "position") == WorldVec3(1.5, -2.25, 3.0));
    CHECK(ReadField<glm::vec3>(transform, "scale") == glm::vec3(2.0f));

    const CompiledPrefabComponent &stats = root.mComponents[1];
    CHECK(ReadField<float>(stats, "speed") == 2.5f);
    CHECK(ReadField<int32_t>(stats, "count") == 7);
    CHECK(ReadField<bool>(stats, "flag"));
    CHECK(ReadField<glm::vec3>(stats, "offset") == glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(ReadField<WorldVec3>(stats, "position") == WorldVec3(10.0, 20.0, 30.0));
    CHECK(ReadString(stats, "label") == "hero");
    CHECK(ReadField<Entity>(stats, "target").IsNull());

    const CompiledPrefabComponent &childStats = ComponentNamed(prefab.Nodes()[1], "PrefabCompileTestStats");
    CHECK(ReadField<float>(childStats, "speed") == 1.25f);
    CHECK(ReadField<int32_t>(childStats, "count") == 0);
    CHECK(ReadField<Entity>(childStats, "target").IsNull());

    REQUIRE(prefab.Relocations().size() == 1);
    CHECK(prefab.Relocations()[0].mNode == 0);
    CHECK(prefab.Relocations()[0].mComponent == 1);
    CHECK(prefab.Relocations()[0].mField == 0);
    CHECK(prefab.Relocations()[0].mTargetNode == 3);
}

TEST_CASE("CompilePrefab warns and skips what this build cannot use", "[prefab]")
{
    mir::RegisterCoreComponents();
    StatsOps();

    PrefabDocument doc = mir::NewPrefabDocument();
    doc.mNodes[0].mComponents.push_back(PrefabComponentInstance{
        "PrefabCompileTestMissing", json::object() });
    doc.mNodes[0].mComponents.push_back(PrefabComponentInstance{
        "PrefabCompileTestStats", json{ { "speed", 1.0 }, { "bogus", 3 } } });

    const PrefabCompileResult result = mir::CompilePrefab(doc, NoLookup());

    REQUIRE(result.mPrefab != nullptr);
    CHECK_FALSE(mir::HasPrefabErrors(result.mDiagnostics));
    CHECK(HasWarningContaining(result.mDiagnostics, "not registered"));
    CHECK(HasWarningContaining(result.mDiagnostics, "unknown field"));
    CHECK(HasWarningContaining(result.mDiagnostics, "bogus"));
    REQUIRE(result.mPrefab->Nodes()[0].mComponents.size() == 1);
    CHECK(ReadField<float>(result.mPrefab->Nodes()[0].mComponents[0], "speed") == 1.0f);
}

TEST_CASE("CompilePrefab rejects invalid templates", "[prefab]")
{
    mir::RegisterCoreComponents();
    StatsOps();

    SECTION("validation errors")
    {
        PrefabDocument doc = mir::NewPrefabDocument();
        doc.mRootNode = 99;

        const PrefabCompileResult result = mir::CompilePrefab(doc, NoLookup());

        CHECK(result.mPrefab == nullptr);
        CHECK(mir::HasPrefabErrors(result.mDiagnostics));
    }
    SECTION("missing nested prefab document")
    {
        PrefabDocument doc = mir::NewPrefabDocument();
        PrefabNode nested;
        nested.mId = 2;
        nested.mKind = PrefabNodeKind::Prefab;
        nested.mParent = doc.mRootNode;
        nested.mPrefab = *mir::ParsePrefabId(kUuidB);
        doc.mNodes.push_back(nested);
        doc.mNextNodeId = 3;

        const PrefabCompileResult result = mir::CompilePrefab(doc, NoLookup());

        CHECK(result.mPrefab == nullptr);
        CHECK(HasErrorContaining(result.mDiagnostics, "not registered"));
    }
    SECTION("multi-segment EntityRef address through an entity")
    {
        PrefabDocument doc = mir::NewPrefabDocument();
        PrefabNode child;
        child.mId = 2;
        child.mParent = doc.mRootNode;
        doc.mNodes.push_back(child);
        doc.mNextNodeId = 3;
        doc.mNodes[0].mComponents.push_back(PrefabComponentInstance{
            "PrefabCompileTestStats", json{ { "target", json::array({ "1", "3" }) } } });

        const PrefabCompileResult result = mir::CompilePrefab(doc, NoLookup());

        CHECK(result.mPrefab == nullptr);
        CHECK(HasErrorContaining(result.mDiagnostics, "not a nested reference"));
    }
    SECTION("undecodable authored field")
    {
        PrefabDocument doc = mir::NewPrefabDocument();
        doc.mNodes[0].mComponents.push_back(PrefabComponentInstance{
            "PrefabCompileTestStats", json{ { "speed", "fast" } } });

        const PrefabCompileResult result = mir::CompilePrefab(doc, NoLookup());

        CHECK(result.mPrefab == nullptr);
        CHECK(HasErrorContaining(result.mDiagnostics, "node 1"));
        CHECK(HasErrorContaining(result.mDiagnostics, "PrefabCompileTestStats"));
        CHECK(HasErrorContaining(result.mDiagnostics, "speed"));
        CHECK(HasErrorContaining(result.mDiagnostics, "float"));
    }
}

TEST_CASE("CompiledPrefabCache keeps the last valid template and bumps revisions", "[prefab]")
{
    mir::RegisterCoreComponents();

    PrefabDocument doc = mir::NewPrefabDocument();
    doc.mNodes[0].mComponents.push_back(PrefabComponentInstance{
        "Transform", json{ { "position", json::array({ 1.0, 2.0, 3.0 }) } } });

    CompiledPrefabCache cache;
    CHECK_FALSE(mir::HasPrefabErrors(cache.Compile(doc, NoLookup())));

    const std::shared_ptr<const CompiledPrefab> first = cache.Find(doc.mId);
    REQUIRE(first != nullptr);
    CHECK(first->Revision() == 1);
    CHECK(ReadField<WorldVec3>(RootTransform(*first), "position") == WorldVec3(1.0, 2.0, 3.0));

    doc.mNodes[0].mComponents[0].mFields["position"] = json::array({ 4.0, 5.0, 6.0 });
    CHECK_FALSE(mir::HasPrefabErrors(cache.Compile(doc, NoLookup())));

    const std::shared_ptr<const CompiledPrefab> second = cache.Find(doc.mId);
    REQUIRE(second != nullptr);
    CHECK(second->Revision() == 2);
    CHECK(ReadField<WorldVec3>(RootTransform(*second), "position") == WorldVec3(4.0, 5.0, 6.0));
    CHECK(first->Revision() == 1);
    CHECK(ReadField<WorldVec3>(RootTransform(*first), "position") == WorldVec3(1.0, 2.0, 3.0));

    doc.mRootNode = 99;
    CHECK(mir::HasPrefabErrors(cache.Compile(doc, NoLookup())));
    CHECK(cache.Find(doc.mId) == second);
    CHECK(cache.Find(doc.mId)->Revision() == 2);

    cache.Remove(doc.mId);
    CHECK(cache.Find(doc.mId) == nullptr);
}

namespace
{
    PrefabDocument InnerDocument()
    {
        json root;
        root["formatVersion"] = 1;
        root["prefabId"] = kUuidB;
        root["nextNodeId"] = "3";
        root["rootNode"] = "1";
        root["nodes"] = json::array({
            json{
                { "id", "1" },
                { "kind", "entity" },
                { "components", json::array({
                    json{ { "type", "PrefabCompileTestStats" }, { "fields", json{
                        { "speed", 2.0 },
                        { "target", json::array({ "2" }) },
                    } } },
                }) },
            },
            json{
                { "id", "2" },
                { "kind", "entity" },
                { "parent", "1" },
                { "components", json::array({
                    json{ { "type", "PrefabCompileTestStats" }, { "fields", json{ { "speed", 3.0 } } } },
                }) },
            },
        });
        return ParseDocument(root);
    }

    PrefabDocument OuterDocument(PrefabId inner, const char *firstPatchTarget = "2")
    {
        json root;
        root["formatVersion"] = 1;
        root["prefabId"] = kUuidA;
        root["nextNodeId"] = "4";
        root["rootNode"] = "1";
        root["nodes"] = json::array({
            json{ { "id", "1" }, { "kind", "entity" } },
            json{
                { "id", "2" },
                { "kind", "prefab" },
                { "parent", "1" },
                { "prefab", mir::ToString(inner) },
                { "patches", json::array({
                    json{
                        { "target", json::array({ firstPatchTarget }) },
                        { "fields", json::array({
                            json{ { "component", "PrefabCompileTestStats" }, { "field", "speed" }, { "value", 5.0 } },
                        }) },
                    },
                }) },
            },
            json{
                { "id", "3" },
                { "kind", "prefab" },
                { "parent", "1" },
                { "prefab", mir::ToString(inner) },
                { "patches", json::array({
                    json{
                        { "target", json::array({ "2" }) },
                        { "fields", json::array({
                            json{ { "component", "PrefabCompileTestStats" }, { "field", "speed" }, { "value", 6.0 } },
                        }) },
                    },
                }) },
            },
        });
        return ParseDocument(root);
    }

    const CompiledPrefabNode &NodeAt(const CompiledPrefab &prefab, const PrefabNodeAddress &address)
    {
        for (const CompiledPrefabNode &node : prefab.Nodes())
        {
            if (node.mAddress == address)
                return node;
        }
        FAIL("node missing");
        return prefab.Nodes().front();
    }
}

TEST_CASE("CompilePrefab expands repeated nested occurrences at their addresses", "[prefab]")
{
    mir::RegisterCoreComponents();
    StatsOps();

    const PrefabDocument inner = InnerDocument();
    const PrefabDocument outer = OuterDocument(inner.mId);
    const std::map<PrefabId, PrefabDocument> documents{ { inner.mId, inner }, { outer.mId, outer } };

    const PrefabCompileResult result = mir::CompilePrefab(outer, LookupOf(documents));

    REQUIRE(result.mPrefab != nullptr);
    CHECK_FALSE(mir::HasPrefabErrors(result.mDiagnostics));
    const CompiledPrefab &prefab = *result.mPrefab;
    REQUIRE(prefab.Nodes().size() == 5);

    const std::array<PrefabNodeAddress, 5> expected{
        PrefabNodeAddress{ 1 },
        PrefabNodeAddress{ 2, 1 },
        PrefabNodeAddress{ 2, 2 },
        PrefabNodeAddress{ 3, 1 },
        PrefabNodeAddress{ 3, 2 },
    };
    for (std::size_t i = 0; i < expected.size(); ++i)
        CHECK(prefab.Nodes()[i].mAddress == expected[i]);

    CHECK_FALSE(prefab.Nodes()[0].mParent.has_value());
    CHECK(prefab.Nodes()[1].mParent == 0);
    CHECK(prefab.Nodes()[2].mParent == 1);
    CHECK(prefab.Nodes()[3].mParent == 0);
    CHECK(prefab.Nodes()[4].mParent == 3);

    CHECK(ReadField<float>(ComponentNamed(NodeAt(prefab, PrefabNodeAddress{ 2, 1 }), "PrefabCompileTestStats"),
        "speed") == 2.0f);
    CHECK(ReadField<float>(ComponentNamed(NodeAt(prefab, PrefabNodeAddress{ 2, 2 }), "PrefabCompileTestStats"),
        "speed") == 5.0f);
    CHECK(ReadField<float>(ComponentNamed(NodeAt(prefab, PrefabNodeAddress{ 3, 2 }), "PrefabCompileTestStats"),
        "speed") == 6.0f);

    REQUIRE(prefab.Relocations().size() == 2);
    CHECK(prefab.Relocations()[0].mNode == 1);
    CHECK(prefab.Relocations()[0].mTargetNode == 2);
    CHECK(prefab.Relocations()[1].mNode == 3);
    CHECK(prefab.Relocations()[1].mTargetNode == 4);

    REQUIRE(prefab.Dependencies().size() == 1);
    CHECK(prefab.Dependencies()[0] == inner.mId);
}

TEST_CASE("A stale nested patch warns and is skipped", "[prefab]")
{
    mir::RegisterCoreComponents();
    StatsOps();

    const PrefabDocument inner = InnerDocument();
    const PrefabDocument outer = OuterDocument(inner.mId, "9");
    const std::map<PrefabId, PrefabDocument> documents{ { inner.mId, inner }, { outer.mId, outer } };

    const PrefabCompileResult result = mir::CompilePrefab(outer, LookupOf(documents));

    REQUIRE(result.mPrefab != nullptr);
    CHECK(HasWarningContaining(result.mDiagnostics, "stale"));
    CHECK(ReadField<float>(ComponentNamed(NodeAt(*result.mPrefab, PrefabNodeAddress{ 2, 2 }),
        "PrefabCompileTestStats"), "speed") == 3.0f);
}

TEST_CASE("Nested compilation rejects cycles, missing documents and excessive expansion", "[prefab]")
{
    mir::RegisterCoreComponents();
    StatsOps();

    SECTION("indirect cycle")
    {
        PrefabDocument first = mir::NewPrefabDocument();
        PrefabDocument second = mir::NewPrefabDocument();

        PrefabNode firstNested;
        firstNested.mId = 2;
        firstNested.mKind = PrefabNodeKind::Prefab;
        firstNested.mParent = 1;
        firstNested.mPrefab = second.mId;
        first.mNodes.push_back(firstNested);
        first.mNextNodeId = 3;

        PrefabNode secondNested;
        secondNested.mId = 2;
        secondNested.mKind = PrefabNodeKind::Prefab;
        secondNested.mParent = 1;
        secondNested.mPrefab = first.mId;
        second.mNodes.push_back(secondNested);
        second.mNextNodeId = 3;

        const std::map<PrefabId, PrefabDocument> documents{ { first.mId, first }, { second.mId, second } };
        const PrefabCompileResult result = mir::CompilePrefab(first, LookupOf(documents));

        CHECK(result.mPrefab == nullptr);
        CHECK(HasErrorContaining(result.mDiagnostics, "is a cycle"));
    }
    SECTION("nesting depth limit")
    {
        std::vector<PrefabDocument> chain;
        std::vector<PrefabId> ids;
        for (std::size_t i = 0; i < mir::kMaxPrefabNestingDepth + 2; ++i)
        {
            chain.push_back(mir::NewPrefabDocument());
            ids.push_back(chain.back().mId);
        }
        for (std::size_t i = 0; i <= mir::kMaxPrefabNestingDepth; ++i)
        {
            PrefabNode nested;
            nested.mId = 2;
            nested.mKind = PrefabNodeKind::Prefab;
            nested.mParent = 1;
            nested.mPrefab = ids[i + 1];
            chain[i].mNodes.push_back(nested);
            chain[i].mNextNodeId = 3;
        }

        std::map<PrefabId, PrefabDocument> documents;
        for (const PrefabDocument &document : chain)
            documents.emplace(document.mId, document);

        const PrefabCompileResult result = mir::CompilePrefab(chain.front(), LookupOf(documents));

        CHECK(result.mPrefab == nullptr);
        CHECK(HasErrorContaining(result.mDiagnostics, "depth limit"));
    }
    SECTION("node limit")
    {
        const PrefabDocument inner = mir::NewPrefabDocument();
        PrefabDocument outer = mir::NewPrefabDocument();
        for (std::size_t i = 0; i < mir::kMaxCompiledPrefabNodes; ++i)
        {
            PrefabNode nested;
            nested.mId = i + 2;
            nested.mKind = PrefabNodeKind::Prefab;
            nested.mParent = 1;
            nested.mPrefab = inner.mId;
            outer.mNodes.push_back(nested);
        }
        outer.mNextNodeId = mir::kMaxCompiledPrefabNodes + 2;

        const std::map<PrefabId, PrefabDocument> documents{ { inner.mId, inner }, { outer.mId, outer } };
        const PrefabCompileResult result = mir::CompilePrefab(outer, LookupOf(documents));

        CHECK(result.mPrefab == nullptr);
        CHECK(HasErrorContaining(result.mDiagnostics, "node limit"));
    }
    SECTION("cache keeps the last valid template")
    {
        const PrefabDocument inner = InnerDocument();
        const PrefabDocument outer = OuterDocument(inner.mId);
        const std::map<PrefabId, PrefabDocument> documents{ { inner.mId, inner }, { outer.mId, outer } };

        CompiledPrefabCache cache;
        REQUIRE_FALSE(mir::HasPrefabErrors(cache.Compile(outer, LookupOf(documents))));
        const std::shared_ptr<const CompiledPrefab> valid = cache.Find(outer.mId);
        REQUIRE(valid != nullptr);

        CHECK(mir::HasPrefabErrors(cache.Compile(outer, NoLookup())));
        CHECK(cache.Find(outer.mId) == valid);
        CHECK(cache.Find(outer.mId)->Revision() == valid->Revision());
    }
}
