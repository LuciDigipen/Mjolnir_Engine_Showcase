/**
 * @file PrefabCoordinator.cpp
 * @author Jackey Cen
 * @brief Publishes compiled prefab edits to the open scene at the Edit safe point.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "prefab/PrefabCoordinator.h"

#include "core/ecs/ComponentFields.h"
#include "core/ecs/ComponentRegistry.h"
#include "core/ecs/EntityChanges.h"
#include "core/ecs/EngineMode.h"
#include "core/ecs/TransformHierarchy.h"
#include "core/ecs/World.h"
#include "core/log/Log.h"
#include "scene/SceneEntityRefs.h"
#include "scene/SceneInstancePatch.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();

        struct alignas(16) FieldBuffer
        {
            std::byte mBytes[64];
        };

        constexpr std::size_t kMaxFieldSize = std::max({
            FieldSize(FieldKind::Bool),
            FieldSize(FieldKind::Int),
            FieldSize(FieldKind::Float),
            FieldSize(FieldKind::Vec3),
            FieldSize(FieldKind::WorldPosition),
            FieldSize(FieldKind::Vec4),
            FieldSize(FieldKind::Quat),
            FieldSize(FieldKind::Mat4),
            FieldSize(FieldKind::EntityRef),
            FieldSize(FieldKind::Handle),
            FieldSize(FieldKind::String),
        });
        static_assert(sizeof(FieldBuffer) >= kMaxFieldSize, "FieldBuffer must hold every FieldKind");

        using NodeIndex = std::map<PrefabNodeAddress, std::size_t>;
        using RelocationIndex = std::map<std::tuple<std::size_t, std::size_t, std::size_t>, std::size_t>;

        NodeIndex IndexNodes(const CompiledPrefab &prefab)
        {
            NodeIndex index;
            const std::span<const CompiledPrefabNode> nodes = prefab.Nodes();
            for (std::size_t i = 0; i < nodes.size(); ++i)
                index.emplace(nodes[i].mAddress, i);
            return index;
        }

        RelocationIndex IndexRelocations(const CompiledPrefab &prefab)
        {
            RelocationIndex index;
            for (const PrefabEntityRefRelocation &relocation : prefab.Relocations())
            {
                index.emplace(
                    std::make_tuple(relocation.mNode, relocation.mComponent, relocation.mField),
                    relocation.mTargetNode);
            }
            return index;
        }

        std::optional<std::size_t> RelocationTarget(const RelocationIndex &index, std::size_t node,
            std::size_t component, std::size_t field)
        {
            const auto it = index.find(std::make_tuple(node, component, field));
            if (it == index.end())
                return std::nullopt;
            return it->second;
        }

        std::size_t FindComponent(const CompiledPrefabNode &node, std::string_view type)
        {
            for (std::size_t i = 0; i < node.mComponents.size(); ++i)
            {
                if (node.mComponents[i].mOps->mType.name == type)
                    return i;
            }
            return kNoIndex;
        }

        // The top root node's Transform is scene placement; a nested occurrence root's is template content.
        bool IsPlacementTransform(const CompiledPrefabNode &node, std::string_view type)
        {
            return !node.mParent.has_value() && type == "Transform";
        }

        std::optional<SceneNodeAddress> RelocationTargetAddress(const RelocationIndex &index,
            std::span<const CompiledPrefabNode> nodes, std::size_t node, std::size_t component, std::size_t field)
        {
            const std::optional<std::size_t> target = RelocationTarget(index, node, component, field);
            if (!target)
                return std::nullopt;
            return nodes[*target].mAddress;
        }

        Entity MemberEntity(const LoadedScene &scene, World &world, const ScenePrefabInstance &instance,
            const SceneNodeAddress &address)
        {
            const auto binding = instance.mBindings.find(address);
            if (binding == instance.mBindings.end())
                return kNullEntity;

            const auto entity = scene.mEntities.find(binding->second);
            if (entity == scene.mEntities.end() || !world.IsAlive(entity->second))
                return kNullEntity;
            return entity->second;
        }

        StableId BindingOf(const ScenePrefabInstance &instance, const SceneNodeAddress &address)
        {
            const auto binding = instance.mBindings.find(address);
            return binding == instance.mBindings.end() ? kNullStableId : binding->second;
        }

        std::shared_ptr<const CompiledPrefab> FindOrCompileTemplate(PrefabRegistry &registry,
            CompiledPrefabCache &cache, PrefabId id)
        {
            if (std::shared_ptr<const CompiledPrefab> cached = cache.Find(id))
                return cached;

            const PrefabDocument *document = registry.Find(id);
            if (document == nullptr)
                return nullptr;
            cache.Compile(*document);
            return cache.Find(id);
        }

        void RecordFieldChange(EntityChangeSet &changes, Entity entity, const ComponentOps &ops,
            std::string_view field)
        {
            for (EntityComponentChange &change : changes.mChangedComponents)
            {
                if (change.mEntity != entity || !(change.mType == ops.mType))
                    continue;
                for (const std::string &name : change.mFields)
                {
                    if (name == field)
                        return;
                }
                change.mFields.emplace_back(field);
                return;
            }

            EntityComponentChange change;
            change.mEntity = entity;
            change.mType = ops.mType;
            change.mFields.emplace_back(field);
            changes.mChangedComponents.push_back(std::move(change));
        }

        void RecordRemovedComponent(EntityChangeSet &changes, Entity entity, const ComponentOps &ops)
        {
            const EntityComponentRef entry{ entity, ops.mType };
            if (std::find(changes.mRemovedComponents.begin(), changes.mRemovedComponents.end(), entry) ==
                changes.mRemovedComponents.end())
            {
                changes.mRemovedComponents.push_back(entry);
            }
        }

        void RecordAddedComponent(EntityChangeSet &changes, Entity entity, const ComponentOps &ops)
        {
            const EntityComponentRef entry{ entity, ops.mType };
            if (std::find(changes.mAddedComponents.begin(), changes.mAddedComponents.end(), entry) ==
                changes.mAddedComponents.end())
            {
                changes.mAddedComponents.push_back(entry);
            }
        }

        bool HasChanges(const EntityChangeSet &changes)
        {
            return !changes.mRemovedEntities.empty() || !changes.mRemovedComponents.empty() ||
                !changes.mChangedComponents.empty() || !changes.mAddedEntities.empty() ||
                !changes.mAddedComponents.empty();
        }

        struct ComponentOp
        {
            Entity mEntity;
            const ComponentOps *mOps = nullptr;
        };

        struct ComponentAddition
        {
            Entity mEntity;
            std::size_t mNewNode = kNoIndex;
            std::size_t mNewComponent = kNoIndex;
        };

        struct FieldWrite
        {
            Entity mEntity;
            const ComponentOps *mOps = nullptr;
            const FieldDesc *mField = nullptr;

            // EntityRef writes resolve this node against the instance's bindings.
            bool mEntityRef = false;
            std::size_t mTargetNode = kNoIndex;
            StableId mOwner = kNullStableId;

            FieldBuffer mValue{};
        };

        struct InstancePlan
        {
            StableId mRoot = kNullStableId;
            ScenePrefabInstance *mInstance = nullptr;
            std::shared_ptr<const CompiledPrefab> mOld;
            std::shared_ptr<const CompiledPrefab> mNew;

            NodeIndex mOldIndex;
            NodeIndex mNewIndex;
            RelocationIndex mNewRelocations;

            std::vector<SceneNodeAddress> mRemovedAddresses;
            std::vector<std::size_t> mAddedNodes;
            std::vector<ComponentOp> mComponentRemovals;
            std::vector<ComponentAddition> mComponentAdditions;
            std::vector<FieldWrite> mFieldWrites;

            std::vector<StableId> mRemovedMemberIds;
            std::vector<Entity> mRemovedRoots;
            std::vector<Entity> mRemovedEntities;
        };

        void PlanEntityRefWrite(InstancePlan &plan, EntityChangeSet &changes, Entity entity, StableId owner,
            std::size_t newNode, std::size_t newComponent, std::size_t fieldIndex, bool recordField = true)
        {
            const CompiledPrefabNode &node = plan.mNew->Nodes()[newNode];
            const CompiledPrefabComponent &component = node.mComponents[newComponent];
            const FieldDesc &field = component.mOps->mFields[fieldIndex];

            FieldWrite write;
            write.mEntity = entity;
            write.mOps = component.mOps;
            write.mField = &field;
            write.mEntityRef = true;
            write.mOwner = owner;
            if (const std::optional<std::size_t> target =
                RelocationTarget(plan.mNewRelocations, newNode, newComponent, fieldIndex))
            {
                write.mTargetNode = *target;
            }

            plan.mFieldWrites.push_back(std::move(write));
            if (recordField)
                RecordFieldChange(changes, entity, *component.mOps, field.mName);
        }

        InstancePlan BuildPlan(World &world, LoadedScene &scene, StableId root, ScenePrefabInstance &instance,
            const std::shared_ptr<const CompiledPrefab> &oldPrefab,
            const std::shared_ptr<const CompiledPrefab> &newPrefab, EntityChangeSet &beforeChanges)
        {
            InstancePlan plan;
            plan.mRoot = root;
            plan.mInstance = &instance;
            plan.mOld = oldPrefab;
            plan.mNew = newPrefab;
            plan.mOldIndex = IndexNodes(*oldPrefab);
            plan.mNewIndex = IndexNodes(*newPrefab);
            plan.mNewRelocations = IndexRelocations(*newPrefab);
            const RelocationIndex oldRelocations = IndexRelocations(*oldPrefab);

            const std::span<const CompiledPrefabNode> oldNodes = oldPrefab->Nodes();
            const std::span<const CompiledPrefabNode> newNodes = newPrefab->Nodes();

            for (const auto &[address, oldIndex] : plan.mOldIndex)
            {
                const auto newIt = plan.mNewIndex.find(address);
                if (newIt == plan.mNewIndex.end())
                {
                    plan.mRemovedAddresses.push_back(address);
                    const StableId member = BindingOf(instance, address);
                    if (member == kNullStableId)
                        continue;

                    plan.mRemovedMemberIds.push_back(member);
                    const auto entityIt = scene.mEntities.find(member);
                    if (entityIt != scene.mEntities.end() && world.IsAlive(entityIt->second))
                    {
                        plan.mRemovedRoots.push_back(entityIt->second);
                        plan.mRemovedEntities.push_back(entityIt->second);
                        ForEachDescendant(world, entityIt->second, [&](Entity descendant)
                            {
                                plan.mRemovedEntities.push_back(descendant);
                            });
                    }
                    continue;
                }

                const std::size_t newIndex = newIt->second;
                const Entity entity = MemberEntity(scene, world, instance, address);
                if (entity.IsNull())
                    continue;

                const StableId owner = BindingOf(instance, address);
                const CompiledPrefabNode &oldNode = oldNodes[oldIndex];
                const CompiledPrefabNode &newNode = newNodes[newIndex];

                for (std::size_t oldComponent = 0; oldComponent < oldNode.mComponents.size(); ++oldComponent)
                {
                    const CompiledPrefabComponent &oldComp = oldNode.mComponents[oldComponent];
                    const ComponentOps &ops = *oldComp.mOps;
                    const std::size_t newComponent = FindComponent(newNode, ops.mType.name);
                    if (newComponent == kNoIndex)
                    {
                        if (IsPlacementTransform(newNode, ops.mType.name))
                            continue;
                        if (ops.Has(world, entity) && FindAddedComponent(instance, address, ops.mType.name) == nullptr)
                        {
                            plan.mComponentRemovals.push_back(ComponentOp{ entity, &ops });
                            RecordRemovedComponent(beforeChanges, entity, ops);
                        }
                        continue;
                    }

                    if (HasRemovedComponent(instance, address, ops.mType.name))
                        continue;
                    if (FindAddedComponent(instance, address, ops.mType.name) != nullptr)
                        continue;

                    const CompiledPrefabComponent &newComp = newNode.mComponents[newComponent];
                    if (!ops.Has(world, entity))
                    {
                        if (IsPlacementTransform(newNode, ops.mType.name))
                            continue;
                        plan.mComponentAdditions.push_back(ComponentAddition{ entity, newIndex, newComponent });
                        RecordAddedComponent(beforeChanges, entity, ops);
                        for (std::size_t fieldIndex = 0; fieldIndex < ops.mFields.size(); ++fieldIndex)
                        {
                            if (ops.mFields[fieldIndex].mKind == FieldKind::EntityRef)
                                PlanEntityRefWrite(plan, beforeChanges, entity, owner, newIndex, newComponent, fieldIndex, false);
                        }
                        continue;
                    }

                    void *liveComponent = ops.GetComponent(world, entity);
                    if (liveComponent == nullptr)
                        continue;

                    if (IsPlacementTransform(newNode, ops.mType.name))
                        continue;

                    for (std::size_t fieldIndex = 0; fieldIndex < ops.mFields.size(); ++fieldIndex)
                    {
                        const FieldDesc &field = ops.mFields[fieldIndex];
                        if (field.ReadOnly())
                            continue;
                        if (FindInstanceFieldPatch(instance, address, ops.mType.name, field.mName) != nullptr)
                            continue;

                        if (field.mKind == FieldKind::EntityRef)
                        {
                            const std::optional<SceneNodeAddress> oldTarget = RelocationTargetAddress(
                                oldRelocations, oldNodes, oldIndex, oldComponent, fieldIndex);
                            const std::optional<SceneNodeAddress> newTarget = RelocationTargetAddress(
                                plan.mNewRelocations, newNodes, newIndex, newComponent, fieldIndex);
                            if (oldTarget == newTarget)
                                continue;

                            FieldWrite write;
                            write.mEntity = entity;
                            write.mOps = &ops;
                            write.mField = &field;
                            write.mEntityRef = true;
                            write.mOwner = owner;
                            if (const std::optional<std::size_t> targetNode = RelocationTarget(plan.mNewRelocations,
                                newIndex, newComponent, fieldIndex))
                            {
                                write.mTargetNode = *targetNode;
                            }
                            plan.mFieldWrites.push_back(std::move(write));
                            RecordFieldChange(beforeChanges, entity, ops, field.mName);
                            continue;
                        }

                        FieldBuffer oldValue{};
                        FieldBuffer newValue{};
                        field.Read(oldComp.mBytes.data(), oldValue.mBytes);
                        field.Read(newComp.mBytes.data(), newValue.mBytes);
                        if (std::memcmp(oldValue.mBytes, newValue.mBytes, FieldSize(field.mKind)) == 0)
                            continue;

                        FieldWrite write;
                        write.mEntity = entity;
                        write.mOps = &ops;
                        write.mField = &field;
                        write.mValue = newValue;
                        plan.mFieldWrites.push_back(std::move(write));
                        RecordFieldChange(beforeChanges, entity, ops, field.mName);
                    }
                }

                for (std::size_t newComponent = 0; newComponent < newNode.mComponents.size(); ++newComponent)
                {
                    const CompiledPrefabComponent &newComp = newNode.mComponents[newComponent];
                    const ComponentOps &ops = *newComp.mOps;
                    if (FindComponent(oldNode, ops.mType.name) != kNoIndex)
                        continue;
                    if (HasRemovedComponent(instance, address, ops.mType.name))
                        continue;
                    if (FindAddedComponent(instance, address, ops.mType.name) != nullptr)
                        continue;
                    if (ops.Has(world, entity))
                        continue;

                    if (IsPlacementTransform(newNode, ops.mType.name))
                        continue;

                    plan.mComponentAdditions.push_back(ComponentAddition{ entity, newIndex, newComponent });
                    RecordAddedComponent(beforeChanges, entity, ops);
                    for (std::size_t fieldIndex = 0; fieldIndex < ops.mFields.size(); ++fieldIndex)
                    {
                        if (ops.mFields[fieldIndex].mKind == FieldKind::EntityRef)
                            PlanEntityRefWrite(plan, beforeChanges, entity, owner, newIndex, newComponent, fieldIndex, false);
                    }
                }
            }

            for (const auto &[address, newIndex] : plan.mNewIndex)
            {
                (void)address;
                if (!plan.mOldIndex.contains(address))
                    plan.mAddedNodes.push_back(newIndex);
            }

            return plan;
        }

        void RemoveBinding(LoadedScene &scene, ScenePrefabInstance &instance, const SceneNodeAddress &address)
        {
            const auto binding = instance.mBindings.find(address);
            if (binding == instance.mBindings.end())
                return;

            scene.mEntities.erase(binding->second);
            instance.mBindings.erase(binding);
        }

        void ApplySiblingOrder(World &world, LoadedScene &scene, const ScenePrefabInstance &instance,
            std::span<const CompiledPrefabNode> newNodes)
        {
            std::map<SceneNodeAddress, std::vector<Entity>> desired;
            for (const CompiledPrefabNode &node : newNodes)
            {
                if (!node.mParent.has_value())
                    continue;
                const Entity entity = MemberEntity(scene, world, instance, node.mAddress);
                if (entity.IsNull())
                    continue;
                desired[newNodes[*node.mParent].mAddress].push_back(entity);
            }

            for (const auto &[parentAddress, children] : desired)
            {
                if (children.size() < 2)
                    continue;
                const Entity parent = MemberEntity(scene, world, instance, parentAddress);
                if (parent.IsNull())
                    continue;

                const std::span<const Entity> current = world.HierarchyIndex().ChildrenOf(parent);
                std::size_t next = 0;
                for (const Entity child : current)
                {
                    if (std::find(children.begin(), children.end(), child) == children.end())
                        continue;
                    if (next >= children.size() || children[next] != child)
                        break;
                    ++next;
                }
                if (next == children.size())
                    continue;

                for (std::size_t i = children.size(); i > 0; --i)
                {
                    const Entity before = i < children.size() ? children[i] : kNullEntity;
                    SetParent(world, children[i - 1], parent, before, false);
                }
            }
        }

        void ApplyPlan(World &world, LoadedScene &scene, InstancePlan &plan, EntityChangeSet &afterChanges)
        {
            ScenePrefabInstance &instance = *plan.mInstance;
            const std::span<const CompiledPrefabNode> newNodes = plan.mNew->Nodes();

            if (!plan.mRemovedAddresses.empty())
                scene.mDirty = true;

            for (const SceneNodeAddress &address : plan.mRemovedAddresses)
            {
                RemoveBinding(scene, instance, address);
                std::erase_if(instance.mPatches, [&](const SceneInstancePatch &patch)
                    {
                        return patch.mTarget == address;
                    });
            }
            for (StableId id : plan.mRemovedMemberIds)
            {
                std::erase_if(scene.mUnresolvedEntityRefs, [&](const auto &entry)
                    {
                        return std::get<0>(entry.first) == id;
                    });
            }

            for (const ComponentOp &removal : plan.mComponentRemovals)
            {
                if (world.IsAlive(removal.mEntity) && removal.mOps->Has(world, removal.mEntity))
                    removal.mOps->Remove(world, removal.mEntity);
            }

            for (const ComponentAddition &addition : plan.mComponentAdditions)
            {
                if (!world.IsAlive(addition.mEntity))
                    continue;
                const CompiledPrefabComponent &component =
                    newNodes[addition.mNewNode].mComponents[addition.mNewComponent];
                component.mOps->AddCopy(world, addition.mEntity, component.mBytes.data());
            }

            for (std::size_t newNodeIndex : plan.mAddedNodes)
            {
                const CompiledPrefabNode &node = newNodes[newNodeIndex];
                const Entity entity = world.CreateEntity();
                const StableId id = AllocateStableId(scene);
                scene.mEntities[id] = entity;
                instance.mBindings[node.mAddress] = id;
                scene.mDirty = true;

                for (const CompiledPrefabComponent &component : node.mComponents)
                {
                    component.mOps->AddCopy(world, entity, component.mBytes.data());
                    afterChanges.mAddedComponents.push_back(EntityComponentRef{ entity, component.mOps->mType });
                }
                afterChanges.mAddedEntities.push_back(entity);
            }

            for (std::size_t newNodeIndex : plan.mAddedNodes)
            {
                const CompiledPrefabNode &node = newNodes[newNodeIndex];
                const Entity entity = MemberEntity(scene, world, instance, node.mAddress);
                if (entity.IsNull())
                    continue;

                for (std::size_t componentIndex = 0; componentIndex < node.mComponents.size(); ++componentIndex)
                {
                    const CompiledPrefabComponent &component = node.mComponents[componentIndex];
                    void *bytes = component.mOps->GetComponent(world, entity);
                    if (bytes == nullptr)
                        continue;

                    for (std::size_t fieldIndex = 0; fieldIndex < component.mOps->mFields.size(); ++fieldIndex)
                    {
                        const FieldDesc &field = component.mOps->mFields[fieldIndex];
                        if (field.mKind != FieldKind::EntityRef)
                            continue;

                        Entity target = kNullEntity;
                        if (const std::optional<std::size_t> targetNode =
                            RelocationTarget(plan.mNewRelocations, newNodeIndex, componentIndex, fieldIndex))
                        {
                            target = MemberEntity(scene, world, instance, newNodes[*targetNode].mAddress);
                        }
                        field.Write(bytes, &target);
                    }
                }
            }

            for (std::size_t newNodeIndex = 0; newNodeIndex < newNodes.size(); ++newNodeIndex)
            {
                const CompiledPrefabNode &newNode = newNodes[newNodeIndex];
                const Entity entity = MemberEntity(scene, world, instance, newNode.mAddress);
                if (entity.IsNull())
                    continue;

                const auto oldIt = plan.mOldIndex.find(newNode.mAddress);
                const bool added = oldIt == plan.mOldIndex.end();

                if (newNode.mParent.has_value())
                {
                    const SceneNodeAddress &parentAddress = newNodes[*newNode.mParent].mAddress;
                    bool reparent = added;
                    if (!added)
                    {
                        const CompiledPrefabNode &oldNode = plan.mOld->Nodes()[oldIt->second];
                        std::optional<SceneNodeAddress> oldParent;
                        if (oldNode.mParent.has_value())
                            oldParent = plan.mOld->Nodes()[*oldNode.mParent].mAddress;
                        reparent = oldParent != parentAddress;
                    }
                    if (!reparent)
                        continue;

                    const Entity parent = MemberEntity(scene, world, instance, parentAddress);
                    if (!parent.IsNull())
                        SetParent(world, entity, parent, kNullEntity, false);
                    continue;
                }

                bool reparent = added;
                if (!added)
                {
                    const CompiledPrefabNode &oldNode = plan.mOld->Nodes()[oldIt->second];
                    reparent = oldNode.mParent.has_value();
                }
                if (!reparent)
                    continue;

                Entity sceneParent = kNullEntity;
                if (instance.mParent != kNullStableId)
                {
                    const auto parentIt = scene.mEntities.find(instance.mParent);
                    if (parentIt != scene.mEntities.end() && world.IsAlive(parentIt->second))
                        sceneParent = parentIt->second;
                }
                SetParent(world, entity, sceneParent, kNullEntity, false);
            }

            ApplySiblingOrder(world, scene, instance, newNodes);

            for (const FieldWrite &write : plan.mFieldWrites)
            {
                if (!world.IsAlive(write.mEntity) || !write.mOps->Has(world, write.mEntity))
                    continue;

                void *component = write.mOps->GetComponent(world, write.mEntity);
                if (component == nullptr)
                    continue;

                if (!write.mEntityRef)
                {
                    write.mField->Write(component, write.mValue.mBytes);
                    continue;
                }

                Entity target = kNullEntity;
                if (write.mTargetNode != kNoIndex)
                {
                    const SceneNodeAddress &targetAddress = newNodes[write.mTargetNode].mAddress;
                    target = MemberEntity(scene, world, instance, targetAddress);
                }
                write.mField->Write(component, &target);
                if (write.mOwner != kNullStableId)
                {
                    scene.mUnresolvedEntityRefs.erase(
                        { write.mOwner, std::string(write.mOps->mType.name), std::string(write.mField->mName) });
                }
            }

            for (const Entity entity : plan.mRemovedRoots)
            {
                if (world.IsAlive(entity))
                    DestroySceneEntity(world, scene, entity);
            }
        }
    }

    PrefabCoordinator::PrefabCoordinator() : mResolver(mRegistry, mCache) {}

    bool PrefabCoordinator::SetDocument(const PrefabDocument &doc, std::vector<PrefabDiagnostic> *diagnostics)
    {
        if (!mRegistry.SetDocument(doc, diagnostics))
            return false;

        mPendingDocuments.insert(doc.mId);
        return true;
    }

    bool PrefabCoordinator::RemoveDocument(PrefabId id)
    {
        mPendingDocuments.erase(id);
        if (!mRegistry.RemoveDocument(id))
            return false;

        mCache.Remove(id);
        std::erase_if(mPublished, [id](const auto &entry) { return entry.second.mPrefab == id; });
        return true;
    }

    void PrefabCoordinator::ScanSources(const std::filesystem::path &prefabsDir)
    {
        const std::vector<PrefabDiagnostic> diagnostics = mRegistry.Scan(prefabsDir);
        for (const PrefabDiagnostic &diagnostic : diagnostics)
        {
            if (diagnostic.mSeverity == PrefabDiagnosticSeverity::Error)
                MIR_LOG_ERROR("Prefab '{}': {}", diagnostic.mPath, diagnostic.mMessage);
            else
                MIR_LOG_WARN("Prefab '{}': {}", diagnostic.mPath, diagnostic.mMessage);
        }
    }

    void PrefabCoordinator::Bind(LoadedScene &scene)
    {
        mScene = &scene;
        RecordPublished(scene);
    }

    void PrefabCoordinator::Commit(World &world)
    {
        if (mScene != nullptr)
            ReconcilePublished(*mScene);

        for (PrefabId id : mPendingDocuments)
        {
            const PrefabDocument *document = mRegistry.Find(id);
            if (document == nullptr)
                continue;

            const std::vector<PrefabDiagnostic> diagnostics = mCache.Compile(*document);
            if (HasPrefabErrors(diagnostics))
                MIR_LOG_WARN("PrefabCoordinator: prefab '{}' did not compile; its instances keep the last template",
                    ToString(id));
        }
        mPendingDocuments.clear();

        if (mScene == nullptr)
            return;

        EntityChangeSet beforeChanges;
        std::vector<InstancePlan> plans;

        for (auto &[root, instance] : mScene->mInstances)
        {
            const std::optional<PrefabId> id = ParsePrefabId(instance.mPrefab);
            if (!id)
                continue;

            const auto published = mPublished.find(root);
            if (published == mPublished.end() || !published->second.mTemplate)
                continue;

            const std::shared_ptr<const CompiledPrefab> current = mCache.Find(*id);
            if (!current || current->Revision() == published->second.mRevision)
                continue;

            plans.push_back(BuildPlan(world, *mScene, root, instance, published->second.mTemplate, current,
                beforeChanges));
        }

        if (plans.empty())
            return;

        mPublishedChanges = true;

        std::set<uint64_t> removedEntityKeys;
        for (const InstancePlan &plan : plans)
        {
            for (Entity entity : plan.mRemovedEntities)
            {
                if (removedEntityKeys.insert(PackEntity(entity)).second)
                    beforeChanges.mRemovedEntities.push_back(entity);
            }
        }

        std::set<StableId> removedIds;
        for (const InstancePlan &plan : plans)
            removedIds.insert(plan.mRemovedMemberIds.begin(), plan.mRemovedMemberIds.end());

        bool maintained = false;
        if (!removedIds.empty())
        {
            const std::vector<StableId> ids(removedIds.begin(), removedIds.end());
            std::vector<ClearedSceneEntityRef> cleared;
            ClearSceneEntityRefsTo(world, *mScene, ids, &cleared);
            maintained = !cleared.empty();
            for (const ClearedSceneEntityRef &entry : cleared)
            {
                if (const ComponentOps *ops = ComponentRegistry::Instance().Find(entry.mType.name))
                    RecordFieldChange(beforeChanges, entry.mEntity, *ops, entry.mField);
            }
        }

        EntityChangeListeners *listeners = world.TryResource<EntityChangeListeners>();
        if (listeners != nullptr && HasChanges(beforeChanges))
            listeners->NotifyBeforeRemove(world, beforeChanges);

        EntityChangeSet afterChanges = beforeChanges;
        for (InstancePlan &plan : plans)
        {
            maintained |= !plan.mRemovedMemberIds.empty() || !plan.mAddedNodes.empty();
            ApplyPlan(world, *mScene, plan, afterChanges);
            mPublished[plan.mRoot] = PublishedInstance{ plan.mNew->Id(), plan.mNew->Revision(), plan.mNew };
        }

        if (maintained)
            mSceneMaintenance = true;

        if (listeners != nullptr && HasChanges(afterChanges))
            listeners->NotifyAfterChange(world, afterChanges);
    }

    bool PrefabCoordinator::ConsumePublishedChanges()
    {
        return std::exchange(mPublishedChanges, false);
    }

    bool PrefabCoordinator::ConsumeSceneMaintenance()
    {
        return std::exchange(mSceneMaintenance, false);
    }

    bool PrefabCoordinator::HasPendingWork() const
    {
        if (!mPendingDocuments.empty())
            return true;
        if (mScene == nullptr)
            return false;

        for (const auto &[root, instance] : mScene->mInstances)
        {
            const auto published = mPublished.find(root);
            if (published == mPublished.end())
                continue;

            const std::optional<PrefabId> id = ParsePrefabId(instance.mPrefab);
            if (!id)
                continue;
            const std::shared_ptr<const CompiledPrefab> current = mCache.Find(*id);
            if (current && current->Revision() != published->second.mRevision)
                return true;
        }
        return false;
    }

    uint64_t PrefabCoordinator::PublishedRevision(StableId root) const
    {
        const auto it = mPublished.find(root);
        return it == mPublished.end() ? 0 : it->second.mRevision;
    }

    void PrefabCoordinator::RecordPublished(const LoadedScene &scene)
    {
        mPublished.clear();
        for (const auto &[root, instance] : scene.mInstances)
        {
            const std::optional<PrefabId> id = ParsePrefabId(instance.mPrefab);
            if (!id)
                continue;

            const std::shared_ptr<const CompiledPrefab> prefab = mCache.Find(*id);
            if (!prefab)
                continue;
            mPublished.emplace(root, PublishedInstance{ *id, prefab->Revision(), prefab });
        }
    }

    void PrefabCoordinator::ReconcilePublished(const LoadedScene &scene)
    {
        std::map<StableId, PublishedInstance> published;
        for (const auto &[root, instance] : scene.mInstances)
        {
            const std::optional<PrefabId> id = ParsePrefabId(instance.mPrefab);
            if (!id)
                continue;

            const auto existing = mPublished.find(root);
            if (existing != mPublished.end() && existing->second.mPrefab == *id)
            {
                published.emplace(root, existing->second);
                continue;
            }

            const std::shared_ptr<const CompiledPrefab> prefab = FindOrCompileTemplate(mRegistry, mCache, *id);
            if (!prefab)
                continue;
            published.emplace(root, PublishedInstance{ *id, prefab->Revision(), prefab });
        }
        mPublished = std::move(published);
    }

    void PrefabSystem::OnUpdate(SystemContext &context)
    {
        if (!IsEditing(context.world))
            return;

        if (PrefabCoordinator *coordinator = context.world.TryResource<PrefabCoordinator>())
            coordinator->Commit(context.world);
    }
}
