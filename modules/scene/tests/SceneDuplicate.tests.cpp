/**
 * @file SceneDuplicate.tests.cpp
 * @author Jackey Cen
 * @brief Tests for scene/SceneDuplicate.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <scene/SceneDuplicate.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/HierarchyIndex.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>
#include <core/math/WorldReal.h>
#include <scene/EntitySnapshot.h>
#include <scene/SceneInstancePatch.h>
#include <scene/SceneIO.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/vec3.hpp>

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

namespace
{
    using nlohmann::json;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::CreateSceneEntity;
    using mir::Entity;
    using mir::FieldKind;
    using mir::FindInstanceFieldPatch;
    using mir::kNullEntity;
    using mir::kNullStableId;
    using mir::LoadedScene;
    using mir::NewScene;
    using mir::RuntimeFieldDecl;
    using mir::SceneComponentRecord;
    using mir::SceneFieldPatch;
    using mir::SceneInstancePatch;
    using mir::SceneNodeAddress;
    using mir::ScenePrefabInstance;
    using mir::StableId;
    using mir::Transform;
    using mir::World;
    using mir::WorldPose;
    using mir::WorldVec3;

    constexpr const char *kPrefabUuid = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";

    const ComponentOps &LinkOps()
    {
        static const RuntimeFieldDecl fields[] = {
            { "target", FieldKind::EntityRef },
            { "other", FieldKind::EntityRef },
        };
        return ComponentRegistry::Instance().RegisterRuntime("SceneDuplicateTestLink", fields);
    }

    const ComponentOps &ExtraOps()
    {
        static const RuntimeFieldDecl fields[] = { { "link", FieldKind::EntityRef } };
        return ComponentRegistry::Instance().RegisterRuntime("SceneDuplicateTestExtra", fields);
    }

    Entity EntityOf(const LoadedScene &scene, StableId id)
    {
        const auto it = scene.mEntities.find(id);
        return it != scene.mEntities.end() ? it->second : kNullEntity;
    }

    std::optional<StableId> StableIdOf(const LoadedScene &scene, Entity entity)
    {
        for (const auto &[id, candidate] : scene.mEntities)
        {
            if (candidate == entity)
                return id;
        }
        return std::nullopt;
    }

    void SetRef(World &world, Entity referrer, const ComponentOps &ops, std::string_view field, Entity target)
    {
        if (!ops.Has(world, referrer))
            ops.AddDefault(world, referrer);
        REQUIRE(ops.FindField(field)->Write(ops.GetComponent(world, referrer), &target));
    }

    Entity ReadRef(World &world, Entity referrer, const ComponentOps &ops, std::string_view field)
    {
        Entity target = kNullEntity;
        const mir::FieldDesc *desc = ops.FindField(field);
        REQUIRE(desc != nullptr);
        desc->Read(ops.GetComponent(world, referrer), &target);
        return target;
    }

    std::vector<Entity> ChildrenOf(World &world, Entity parent)
    {
        std::vector<Entity> children;
        mir::ForEachChild(world, parent, [&](Entity child) { children.push_back(child); });
        return children;
    }

    std::vector<Entity> RootsOf(World &world)
    {
        const std::span<const Entity> roots = world.Resource<mir::HierarchyIndex>().Roots();
        return std::vector<Entity>(roots.begin(), roots.end());
    }

    bool BindsMember(const LoadedScene &scene, StableId member)
    {
        for (const auto &[root, instance] : scene.mInstances)
        {
            (void)root;
            for (const auto &[address, id] : instance.mBindings)
            {
                (void)address;
                if (id == member)
                    return true;
            }
        }
        return false;
    }

    const json *FieldPatchValue(const ScenePrefabInstance &instance, const SceneNodeAddress &target,
        std::string_view component, std::string_view field)
    {
        const SceneFieldPatch *patch = FindInstanceFieldPatch(instance, target, component, field);
        return patch != nullptr ? &patch->mValue : nullptr;
    }

    struct InstanceScene
    {
        World mWorld;
        LoadedScene mScene = NewScene("test");

        Entity mOutside;
        Entity mBeforeRoot;
        Entity mAfterRoot;
        Entity mRoot;
        Entity mMid;
        Entity mOther;
        Entity mGrand;
        Entity mOrdinaryChild;

        StableId mRootId = kNullStableId;
        StableId mMidId = kNullStableId;
        StableId mOtherId = kNullStableId;
        StableId mOutsideId = kNullStableId;

        InstanceScene()
        {
            mir::RegisterCoreComponents();
            LinkOps();
            ExtraOps();

            mOutside = CreateSceneEntity(mWorld, mScene);
            mir::AddTransform(mWorld, mOutside, Transform{ WorldVec3(0.0, 1.0, 0.0) });

            mBeforeRoot = CreateSceneEntity(mWorld, mScene);
            mir::AddTransform(mWorld, mBeforeRoot);

            mRoot = CreateSceneEntity(mWorld, mScene);
            mir::AddTransform(mWorld, mRoot, Transform{ WorldVec3(10.0, 0.0, 0.0) });
            mMid = CreateSceneEntity(mWorld, mScene, mRoot);
            mir::AddTransform(mWorld, mMid, Transform{ WorldVec3(2.0, 1.0, 0.0) }, mRoot);
            mOther = CreateSceneEntity(mWorld, mScene, mRoot);
            mir::AddTransform(mWorld, mOther, Transform{ WorldVec3(0.0, 3.0, 0.0) }, mRoot);
            mGrand = CreateSceneEntity(mWorld, mScene, mMid);
            mir::AddTransform(mWorld, mGrand, Transform{ WorldVec3(0.0, 0.0, 4.0) }, mMid);
            mOrdinaryChild = CreateSceneEntity(mWorld, mScene, mMid);
            mir::AddTransform(mWorld, mOrdinaryChild, Transform{ WorldVec3(1.0, 0.0, 0.0) }, mMid);

            mAfterRoot = CreateSceneEntity(mWorld, mScene);
            mir::AddTransform(mWorld, mAfterRoot);

            SetRef(mWorld, mRoot, LinkOps(), "target", mMid);
            SetRef(mWorld, mRoot, LinkOps(), "other", mOutside);
            ExtraOps().AddDefault(mWorld, mRoot);
            SetRef(mWorld, mRoot, ExtraOps(), "link", mMid);
            SetRef(mWorld, mMid, LinkOps(), "target", mGrand);
            SetRef(mWorld, mMid, LinkOps(), "other", mOutside);

            mRootId = *StableIdOf(mScene, mRoot);
            mMidId = *StableIdOf(mScene, mMid);
            mOtherId = *StableIdOf(mScene, mOther);
            mOutsideId = *StableIdOf(mScene, mOutside);

            ScenePrefabInstance instance;
            instance.mPrefab = kPrefabUuid;
            instance.mRoot = mRootId;
            instance.mSiblingIndex = 2;
            instance.mPlacement = mir::InstancePlacementFromWorld(mWorld, mRoot);
            instance.mBindings[SceneNodeAddress{ 1 }] = mRootId;
            instance.mBindings[SceneNodeAddress{ 2 }] = mMidId;
            instance.mBindings[SceneNodeAddress{ 3 }] = mOtherId;

            SceneInstancePatch patch;
            patch.mTarget = SceneNodeAddress{ 1 };
            patch.mFields.push_back(SceneFieldPatch{
                "SceneDuplicateTestLink", "target", json(mMidId) });
            patch.mFields.push_back(SceneFieldPatch{
                "SceneDuplicateTestLink", "other", json(mOutsideId) });
            SceneComponentRecord added;
            added.mType = "SceneDuplicateTestExtra";
            added.mFields["link"] = json(mMidId);
            patch.mAddedComponents.push_back(std::move(added));
            instance.mPatches.push_back(std::move(patch));

            mScene.mInstances[mRootId] = std::move(instance);
        }

        const ScenePrefabInstance &Instance() const { return mScene.mInstances.at(mRootId); }

        Entity EntityAt(StableId id) const { return EntityOf(mScene, id); }
    };
}

TEST_CASE("Duplicating an instance root keeps the Prefab link and remaps its members", "[scene][duplicate]")
{
    InstanceScene fixture;
    World &world = fixture.mWorld;
    LoadedScene &scene = fixture.mScene;

    const Entity copy = mir::DuplicateSceneEntitySubtree(world, scene, fixture.mRoot);
    REQUIRE(world.IsAlive(copy));

    const std::optional<StableId> copyId = StableIdOf(scene, copy);
    REQUIRE(copyId.has_value());
    REQUIRE(scene.mInstances.contains(*copyId));

    const ScenePrefabInstance &copyRecord = scene.mInstances.at(*copyId);
    CHECK(copyRecord.mPrefab == kPrefabUuid);
    CHECK(copyRecord.mRoot == *copyId);
    CHECK(copyRecord.mBindings.size() == fixture.Instance().mBindings.size());
    CHECK(copyRecord.mSiblingIndex == fixture.Instance().mSiblingIndex + 1);
    CHECK(copyRecord.mParent == fixture.Instance().mParent);
    CHECK(copyRecord.mPlacement == fixture.Instance().mPlacement);
    CHECK(mir::ParentOf(world, copy) == mir::ParentOf(world, fixture.mRoot));
    CHECK(RootsOf(world) == std::vector<Entity>{ fixture.mOutside, fixture.mBeforeRoot, fixture.mRoot, copy,
        fixture.mAfterRoot });

    const StableId midCopy = copyRecord.mBindings.at(SceneNodeAddress{ 2 });
    const StableId otherCopy = copyRecord.mBindings.at(SceneNodeAddress{ 3 });
    CHECK(midCopy != fixture.mMidId);
    CHECK(otherCopy != fixture.mOtherId);
    CHECK(midCopy != copyRecord.mBindings.at(SceneNodeAddress{ 1 }));
    CHECK(fixture.EntityAt(midCopy) != fixture.mMid);
    CHECK(mir::ParentOf(world, fixture.EntityAt(midCopy)) == copy);
    CHECK(mir::ParentOf(world, fixture.EntityAt(otherCopy)) == copy);

    CHECK(ReadRef(world, copy, LinkOps(), "target") == fixture.EntityAt(midCopy));
    CHECK(ReadRef(world, copy, LinkOps(), "other") == fixture.mOutside);
    const Entity midCopyTarget = ReadRef(world, fixture.EntityAt(midCopy), LinkOps(), "target");
    CHECK(midCopyTarget != fixture.mGrand);
    CHECK(mir::IsAncestorOf(world, copy, midCopyTarget));

    const json *target = FieldPatchValue(copyRecord, SceneNodeAddress{ 1 }, "SceneDuplicateTestLink", "target");
    REQUIRE(target != nullptr);
    CHECK(*target == json(midCopy));

    const json *other = FieldPatchValue(copyRecord, SceneNodeAddress{ 1 }, "SceneDuplicateTestLink", "other");
    REQUIRE(other != nullptr);
    CHECK(*other == json(fixture.mOutsideId));

    const SceneInstancePatch *copyPatch = mir::FindInstancePatch(copyRecord, SceneNodeAddress{ 1 });
    REQUIRE(copyPatch != nullptr);
    REQUIRE(copyPatch->mAddedComponents.size() == 1);
    CHECK(copyPatch->mAddedComponents.front().mFields.at("link") == json(midCopy));

    CHECK(scene.mDirty);

    CHECK(ReadRef(world, fixture.mRoot, LinkOps(), "target") == fixture.mMid);
    CHECK(ReadRef(world, fixture.mRoot, LinkOps(), "other") == fixture.mOutside);
    CHECK(ReadRef(world, fixture.mMid, LinkOps(), "target") == fixture.mGrand);
    CHECK(scene.mInstances.at(fixture.mRootId).mBindings.at(SceneNodeAddress{ 2 }) == fixture.mMidId);
}

TEST_CASE("Duplicating an instance member is refused and changes nothing", "[scene][duplicate]")
{
    InstanceScene fixture;
    const std::size_t entities = fixture.mScene.mEntities.size();
    const std::size_t instances = fixture.mScene.mInstances.size();
    const std::size_t midChildren = ChildrenOf(fixture.mWorld, fixture.mMid).size();

    CHECK(mir::DuplicateSceneEntitySubtree(fixture.mWorld, fixture.mScene, fixture.mMid).IsNull());

    CHECK(fixture.mScene.mEntities.size() == entities);
    CHECK(fixture.mScene.mInstances.size() == instances);
    CHECK(ChildrenOf(fixture.mWorld, fixture.mMid).size() == midChildren);
}

TEST_CASE("Copying an instance root as ordinary keeps its world pose and references", "[scene][duplicate]")
{
    InstanceScene fixture;
    World &world = fixture.mWorld;
    LoadedScene &scene = fixture.mScene;

    const std::vector<Entity> sourceMembers = mir::SnapshotMembers(world, fixture.mRoot);
    const WorldPose sourcePose = mir::ResolveWorld(world, fixture.mRoot);

    const Entity copy = mir::CopyInstanceMemberAsOrdinary(world, scene, fixture.mRoot);
    REQUIRE(world.IsAlive(copy));

    const std::optional<StableId> copyId = StableIdOf(scene, copy);
    REQUIRE(copyId.has_value());
    CHECK_FALSE(scene.mInstances.contains(*copyId));
    CHECK_FALSE(BindsMember(scene, *copyId));
    CHECK(mir::SnapshotMembers(world, copy).size() == sourceMembers.size());

    CHECK(RootsOf(world) == std::vector<Entity>{ fixture.mOutside, fixture.mBeforeRoot, fixture.mRoot, copy,
        fixture.mAfterRoot });
    CHECK(mir::ParentOf(world, copy) == mir::ParentOf(world, fixture.mRoot));
    CHECK(mir::ResolveWorld(world, copy).position.Meters() == sourcePose.position.Meters());

    const Entity target = ReadRef(world, copy, LinkOps(), "target");
    CHECK(target != fixture.mMid);
    CHECK(mir::IsAncestorOf(world, copy, target));
    CHECK(ReadRef(world, copy, LinkOps(), "other") == fixture.mOutside);
    CHECK(ReadRef(world, copy, ExtraOps(), "link") == target);

    CHECK(ReadRef(world, fixture.mRoot, LinkOps(), "target") == fixture.mMid);
    CHECK(scene.mInstances.contains(fixture.mRootId));
    CHECK(scene.mInstances.at(fixture.mRootId).mBindings.size() == 3);
}

TEST_CASE("Copying a deep instance member as ordinary sits beside the instance root", "[scene][duplicate]")
{
    InstanceScene fixture;
    World &world = fixture.mWorld;
    LoadedScene &scene = fixture.mScene;

    const WorldPose sourcePose = mir::ResolveWorld(world, fixture.mMid);
    const std::size_t memberCount = mir::SnapshotMembers(world, fixture.mMid).size();

    const Entity copy = mir::CopyInstanceMemberAsOrdinary(world, scene, fixture.mMid);
    REQUIRE(world.IsAlive(copy));

    const std::optional<StableId> copyId = StableIdOf(scene, copy);
    REQUIRE(copyId.has_value());
    CHECK_FALSE(BindsMember(scene, *copyId));
    CHECK(mir::SnapshotMembers(world, copy).size() == memberCount);

    CHECK(RootsOf(world) == std::vector<Entity>{ fixture.mOutside, fixture.mBeforeRoot, fixture.mRoot, copy,
        fixture.mAfterRoot });
    CHECK(mir::ParentOf(world, copy) == mir::ParentOf(world, fixture.mRoot));
    CHECK(mir::ResolveWorld(world, copy).position.Meters() == sourcePose.position.Meters());

    const Entity target = ReadRef(world, copy, LinkOps(), "target");
    CHECK(target != fixture.mGrand);
    CHECK(mir::IsAncestorOf(world, copy, target));
    CHECK(ReadRef(world, copy, LinkOps(), "other") == fixture.mOutside);

    CHECK(mir::ParentOf(world, fixture.mMid) == fixture.mRoot);
    CHECK(ReadRef(world, fixture.mMid, LinkOps(), "target") == fixture.mGrand);
    CHECK(scene.mInstances.at(fixture.mRootId).mBindings.at(SceneNodeAddress{ 2 }) == fixture.mMidId);
}
