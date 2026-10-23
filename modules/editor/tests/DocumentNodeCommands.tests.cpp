/**
 * @file DocumentNodeCommands.tests.cpp
 * @author Jackey Cen
 * @brief Tests for editor/DocumentNodeCommands.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/history/DocumentNodeCommands.h>

#include <editor/Selection.h>
#include <editor/history/AuthoredView.h>
#include <editor/history/Authoring.h>
#include <editor/history/History.h>
#include <editor/history/SceneOps.h>
#include <editor/panels/HierarchyPanel.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EntityPresetRegistry.h>
#include <core/ecs/HierarchyIndex.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using mir::AuthoredAction;
    using mir::AuthoredComponentState;
    using mir::AuthoredEntry;
    using mir::AuthoredFieldState;
    using mir::AuthoredInstanceRole;
    using mir::AuthoredKey;
    using mir::CapturedSubtree;
    using mir::DocumentId;
    using mir::Entity;
    using mir::EntityAction;
    using mir::EntityActionKind;
    using mir::FieldKind;
    using mir::History;
    using mir::ICommand;
    using mir::kNullEntity;
    using mir::World;

    const DocumentId kDocument{ 11, 22 };

    class FakeAuthored final : public mir::IAuthoredEntities
    {
    public:
        std::optional<AuthoredKey> KeyOf(Entity, std::string_view, std::string_view) override
        {
            return std::nullopt;
        }

        std::optional<AuthoredKey> ComponentKeyOf(Entity, std::string_view) override { return std::nullopt; }
        AuthoredFieldState FieldState(const AuthoredKey &) override { return AuthoredFieldState::Inherited; }
        AuthoredComponentState ComponentState(Entity, std::string_view) override
        {
            return AuthoredComponentState::Absent;
        }

        std::vector<std::string> RemovedComponents(Entity) override { return {}; }
        AuthoredInstanceRole InstanceRole(Entity) override { return AuthoredInstanceRole::Ordinary; }
        std::string EncodeField(FieldKind, const void *) override { return "null"; }
        AuthoredAction Revert(const AuthoredKey &) override { return {}; }
        AuthoredAction Apply(const AuthoredKey &) override { return {}; }

        AuthoredAction CreateNode(Entity parent, std::size_t siblingIndex,
            const mir::EntityPresetDesc *preset) override
        {
            ++mCreateCalls;
            mParent = parent;
            mSiblingIndex = siblingIndex;
            mPreset = preset;
            return NodeAction("Create Node", kCreateAddress);
        }

        AuthoredAction PlacePrefabNode(std::string_view contentPath, Entity parent, std::size_t siblingIndex,
            const std::optional<mir::WorldVec3> &placement) override
        {
            ++mPlaceCalls;
            mPrefabPath.assign(contentPath);
            mParent = parent;
            mSiblingIndex = siblingIndex;
            mPlacement = placement;
            return NodeAction("Place Prefab", kPlaceAddress);
        }

        AuthoredAction DeleteNodes(std::span<const Entity> nodes) override
        {
            ++mDeleteCalls;
            mDeleted.assign(nodes.begin(), nodes.end());
            return NodeAction("Delete Node", kDeleteAddress);
        }

        AuthoredAction ReparentNode(Entity node, Entity parent, std::size_t siblingIndex) override
        {
            ++mReparentCalls;
            mReparented = node;
            mParent = parent;
            mSiblingIndex = siblingIndex;
            return NodeAction("Reparent Node", kReparentAddress);
        }

        AuthoredAction CreatePrefab(Entity) override { return {}; }

        bool WritesComponentDirectly(Entity, std::string_view) override { return false; }

        int mCreateCalls = 0;
        int mDeleteCalls = 0;
        int mReparentCalls = 0;
        int mPlaceCalls = 0;
        Entity mParent = kNullEntity;
        Entity mReparented = kNullEntity;
        const mir::EntityPresetDesc *mPreset = nullptr;
        std::size_t mSiblingIndex = 0;
        std::string mPrefabPath;
        std::optional<mir::WorldVec3> mPlacement;
        std::vector<Entity> mDeleted;
        bool mRefuse = false;

    private:
        static constexpr std::uint64_t kCreateAddress = 100;
        static constexpr std::uint64_t kDeleteAddress = 200;
        static constexpr std::uint64_t kReparentAddress = 300;
        static constexpr std::uint64_t kPlaceAddress = 400;

        AuthoredAction NodeAction(std::string name, std::uint64_t address) const
        {
            AuthoredAction action;
            if (mRefuse)
            {
                action.mAllowed = false;
                action.mReason = "the document refused the action";
                action.mName = std::move(name);
                return action;
            }

            action.mName = std::move(name);
            AuthoredKey key;
            key.mObject.mDocument = kDocument;
            key.mObject.mAddress = { address };
            action.mEntries.push_back(AuthoredEntry{ key, std::string("{}") });
            return action;
        }
    };

    class FakeBackend final : public mir::IAuthoringBackend
    {
    public:
        std::optional<std::vector<AuthoredEntry>> Read(std::span<const AuthoredKey> keys) override
        {
            std::vector<AuthoredEntry> entries;
            entries.reserve(keys.size());
            for (const AuthoredKey &key : keys)
                entries.push_back(AuthoredEntry{ key, std::string("before") });
            return entries;
        }

        mir::AuthoringWrite Write(std::span<const AuthoredEntry> entries) override
        {
            mWrites.assign(entries.begin(), entries.end());
            ++mWriteCount;
            return mir::AuthoringWrite{ .mApplied = true };
        }

        int mWriteCount = 0;
        std::vector<AuthoredEntry> mWrites;
    };

    class NoSceneOps final : public mir::ISceneOps
    {
    public:
        Entity CreateEntity() override { return kNullEntity; }
        Entity Duplicate(Entity) override { return kNullEntity; }
        Entity CopyAsOrdinaryObject(Entity) override { return kNullEntity; }
        CapturedSubtree Capture(Entity) override { return {}; }
        std::vector<Entity> Restore(const CapturedSubtree &, Entity, Entity) override { return {}; }
        bool Destroy(Entity) override { return false; }
        Entity PlacePrefab(std::string_view, Entity, const mir::WorldVec3 &) override { return kNullEntity; }
    };

    struct Fixture
    {
        World mWorld;
        FakeAuthored mAuthored;
        FakeBackend mBackend;
        History mHistory;
        NoSceneOps mSceneOps;
        Entity mRoot = kNullEntity;
        Entity mChild = kNullEntity;
        Entity mOther = kNullEntity;

        Fixture()
        {
            mir::RegisterCoreComponents();
            mWorld.EmplaceResource<mir::Selection>();

            mRoot = mWorld.CreateEntity();
            mChild = mWorld.CreateEntity();
            mOther = mWorld.CreateEntity();
            REQUIRE(mir::SetParent(mWorld, mChild, mRoot));
            REQUIRE(mir::SetParent(mWorld, mOther, mRoot));
            mHistory.SetAuthoringBackend(&mBackend);
        }

        void Run(std::unique_ptr<ICommand> command)
        {
            mHistory.Submit(std::move(command));
            mHistory.Flush(mWorld, mSceneOps);
        }
    };
}

TEST_CASE("Hierarchy create in a document authors a node under its root", "[editor][prefabs]")
{
    Fixture fixture;
    const mir::HierarchyIndex &index = fixture.mWorld.HierarchyIndex();
    const std::size_t children = index.ChildrenOf(fixture.mRoot).size();

    fixture.Run(mir::MakeDocumentEntityCommand(
        EntityAction{ .mKind = EntityActionKind::Create }, fixture.mAuthored, fixture.mWorld));

    CHECK(fixture.mAuthored.mCreateCalls == 1);
    CHECK(fixture.mAuthored.mParent == fixture.mRoot);
    CHECK(fixture.mAuthored.mSiblingIndex == children);
    CHECK(fixture.mBackend.mWriteCount == 1);
    CHECK(fixture.mHistory.StepCount() == 1);
    CHECK(fixture.mHistory.IsUnsaved(kDocument));

    fixture.mHistory.RequestUndo();
    fixture.mHistory.Flush(fixture.mWorld, fixture.mSceneOps);
    CHECK(fixture.mBackend.mWriteCount == 2);
    CHECK(fixture.mBackend.mWrites.size() == 1);
}

TEST_CASE("Hierarchy create in a document forwards its preset", "[editor][prefabs]")
{
    Fixture fixture;
    static const mir::EntityPresetDesc preset{ "Test/Preset", nullptr, nullptr };

    fixture.Run(mir::MakeDocumentEntityCommand(
        EntityAction{ .mKind = EntityActionKind::Create, .mPreset = &preset }, fixture.mAuthored, fixture.mWorld));

    CHECK(fixture.mAuthored.mCreateCalls == 1);
    CHECK(fixture.mAuthored.mPreset == &preset);
}

TEST_CASE("Hierarchy delete in a document deletes the selected node", "[editor][prefabs]")
{
    Fixture fixture;
    fixture.Run(mir::MakeDocumentEntityCommand(
        EntityAction{ .mKind = EntityActionKind::Delete, .mTarget = fixture.mChild }, fixture.mAuthored,
        fixture.mWorld));

    CHECK(fixture.mAuthored.mDeleteCalls == 1);
    CHECK(fixture.mAuthored.mDeleted == std::vector<Entity>{ fixture.mChild });
    CHECK(fixture.mHistory.StepCount() == 1);
}

TEST_CASE("Hierarchy reparent in a document moves onto the drop target", "[editor][prefabs]")
{
    Fixture fixture;
    fixture.Run(mir::MakeDocumentEntityCommand(
        EntityAction{ .mKind = EntityActionKind::Reparent, .mTargets = { fixture.mChild },
            .mParent = fixture.mOther },
        fixture.mAuthored, fixture.mWorld));

    CHECK(fixture.mAuthored.mReparentCalls == 1);
    CHECK(fixture.mAuthored.mReparented == fixture.mChild);
    CHECK(fixture.mAuthored.mParent == fixture.mOther);
    CHECK(fixture.mAuthored.mSiblingIndex == 0);
    CHECK(fixture.mHistory.StepCount() == 1);
}

TEST_CASE("A drop on the hierarchy root reparents under the document root", "[editor][prefabs]")
{
    Fixture fixture;
    const mir::HierarchyIndex &index = fixture.mWorld.HierarchyIndex();
    const std::size_t children = index.ChildrenOf(fixture.mRoot).size();

    fixture.Run(mir::MakeDocumentEntityCommand(
        EntityAction{ .mKind = EntityActionKind::Reparent, .mTargets = { fixture.mChild } }, fixture.mAuthored,
        fixture.mWorld));

    CHECK(fixture.mAuthored.mParent == fixture.mRoot);
    CHECK(fixture.mAuthored.mSiblingIndex == children - 1);
}

TEST_CASE("A content Prefab drop in a document places an occurrence under the drop target", "[editor][prefabs]")
{
    Fixture fixture;
    const std::size_t children = fixture.mWorld.HierarchyIndex().ChildrenOf(fixture.mOther).size();

    fixture.Run(mir::MakeDocumentEntityCommand(
        EntityAction{ .mKind = EntityActionKind::PlacePrefab, .mParent = fixture.mOther,
            .mPrefabPath = "assets/prefabs/Chair.prefab" },
        fixture.mAuthored, fixture.mWorld));

    CHECK(fixture.mAuthored.mPlaceCalls == 1);
    CHECK(fixture.mAuthored.mPrefabPath == "assets/prefabs/Chair.prefab");
    CHECK(fixture.mAuthored.mParent == fixture.mOther);
    CHECK(fixture.mAuthored.mSiblingIndex == children);
    CHECK_FALSE(fixture.mAuthored.mPlacement.has_value());
    CHECK(fixture.mHistory.StepCount() == 1);

    const mir::WorldVec3 position(2.0, 0.0, 3.0);
    fixture.Run(mir::MakeDocumentEntityCommand(
        EntityAction{ .mKind = EntityActionKind::PlacePrefab, .mPrefabPath = "assets/prefabs/Chair.prefab",
            .mPlacement = position },
        fixture.mAuthored, fixture.mWorld));

    CHECK(fixture.mAuthored.mPlaceCalls == 2);
    CHECK(fixture.mAuthored.mParent == fixture.mRoot);
    CHECK(fixture.mAuthored.mPlacement == position);
}

TEST_CASE("A refused document Prefab drop leaves History and the document alone", "[editor][prefabs]")
{
    Fixture fixture;
    fixture.mAuthored.mRefuse = true;
    CHECK(mir::MakeDocumentEntityCommand(
        EntityAction{ .mKind = EntityActionKind::PlacePrefab, .mPrefabPath = "assets/prefabs/Chair.prefab" },
        fixture.mAuthored, fixture.mWorld) == nullptr);
    CHECK(fixture.mHistory.StepCount() == 0);
    CHECK(fixture.mAuthored.mPlaceCalls == 1);
}

TEST_CASE("A document does not offer duplicate, copy as ordinary or a refused action", "[editor][prefabs]")
{
    Fixture fixture;
    CHECK(mir::MakeDocumentEntityCommand(
        EntityAction{ .mKind = EntityActionKind::Duplicate, .mTarget = fixture.mChild }, fixture.mAuthored,
        fixture.mWorld) == nullptr);
    CHECK(mir::MakeDocumentEntityCommand(
        EntityAction{ .mKind = EntityActionKind::CopyAsOrdinaryObject, .mTarget = fixture.mChild },
        fixture.mAuthored, fixture.mWorld) == nullptr);
    CHECK(fixture.mHistory.StepCount() == 0);
    CHECK(fixture.mAuthored.mCreateCalls == 0);
    CHECK(fixture.mAuthored.mDeleteCalls == 0);
    CHECK(fixture.mAuthored.mReparentCalls == 0);

    fixture.mAuthored.mRefuse = true;
    CHECK(mir::MakeDocumentEntityCommand(
        EntityAction{ .mKind = EntityActionKind::Create }, fixture.mAuthored, fixture.mWorld) == nullptr);
    CHECK(fixture.mHistory.StepCount() == 0);
}
