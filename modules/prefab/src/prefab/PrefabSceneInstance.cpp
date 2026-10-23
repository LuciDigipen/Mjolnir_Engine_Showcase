/**
 * @file PrefabSceneInstance.cpp
 * @author Jackey Cen
 * @brief Scene prefab resolver and placement built on the compiled prefab graph.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "prefab/PrefabSceneInstance.h"

#include "core/ecs/ComponentFields.h"
#include "core/ecs/ComponentRegistry.h"
#include "core/ecs/TransformHierarchy.h"
#include "core/ecs/World.h"
#include "core/log/Log.h"
#include "prefab/PrefabInstantiate.h"

#include <cstddef>
#include <string>
#include <unordered_set>
#include <utility>

namespace mir
{
    namespace
    {
        uint32_t SiblingIndexInScene(World &world, Entity entity, const LoadedScene &scene)
        {
            std::unordered_set<uint64_t> sceneEntities;
            sceneEntities.reserve(scene.mEntities.size());
            for (const auto &[id, member] : scene.mEntities)
                sceneEntities.insert(PackEntity(member));

            uint32_t position = 0;
            for (const Entity sibling : world.HierarchyIndex().SiblingsOf(entity))
            {
                if (sibling == entity)
                    break;
                if (sceneEntities.contains(PackEntity(sibling)))
                    ++position;
            }
            return position;
        }
    }

    PrefabSceneResolver::PrefabSceneResolver(const PrefabRegistry &registry, CompiledPrefabCache &cache)
        : mRegistry(registry)
        , mCache(cache)
    {
    }

    std::shared_ptr<const CompiledPrefab> PrefabSceneResolver::FindOrCompile(
        const ScenePrefabInstance &instance) const
    {
        const std::optional<PrefabId> id = ParsePrefabId(instance.mPrefab);
        if (!id)
        {
            MIR_LOG_WARN("PrefabSceneResolver: instance has an invalid prefab id '{}'", instance.mPrefab);
            return nullptr;
        }

        if (std::shared_ptr<const CompiledPrefab> cached = mCache.Find(*id))
            return cached;

        const PrefabDocument *document = mRegistry.Find(*id);
        if (document == nullptr)
        {
            MIR_LOG_WARN("PrefabSceneResolver: prefab '{}' is not registered", ToString(*id));
            return nullptr;
        }

        const std::vector<PrefabDiagnostic> diagnostics = mCache.Compile(*document, RegistryLookup(mRegistry));
        if (HasPrefabErrors(diagnostics))
        {
            MIR_LOG_WARN("PrefabSceneResolver: prefab '{}' did not compile", ToString(*id));
            return nullptr;
        }

        std::shared_ptr<const CompiledPrefab> compiled = mCache.Find(*id);
        if (!compiled)
            MIR_LOG_WARN("PrefabSceneResolver: prefab '{}' compiled to nothing", ToString(*id));
        return compiled;
    }

    std::map<SceneNodeAddress, Entity> PrefabSceneResolver::Materialize(World &world,
        const ScenePrefabInstance &instance)
    {
        const std::shared_ptr<const CompiledPrefab> prefab = FindOrCompile(instance);
        if (!prefab)
            return {};

        return InstantiatePrefab(world, *prefab).mEntities;
    }

    bool PrefabSceneResolver::TemplateNodes(const ScenePrefabInstance &instance,
        std::set<SceneNodeAddress> &outNodes) const
    {
        outNodes.clear();

        const std::shared_ptr<const CompiledPrefab> prefab = FindOrCompile(instance);
        if (!prefab)
            return false;

        for (const CompiledPrefabNode &node : prefab->Nodes())
            outNodes.insert(node.mAddress);
        return true;
    }

    bool PrefabSceneResolver::TemplateEntityRef(const ScenePrefabInstance &instance, const SceneNodeAddress &node,
        std::string_view component, std::string_view field, std::optional<SceneNodeAddress> &outTarget) const
    {
        outTarget.reset();

        const std::shared_ptr<const CompiledPrefab> prefab = FindOrCompile(instance);
        if (!prefab)
            return false;

        const std::span<const CompiledPrefabNode> nodes = prefab->Nodes();
        std::size_t nodeIndex = nodes.size();
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            if (nodes[i].mAddress == node)
            {
                nodeIndex = i;
                break;
            }
        }
        if (nodeIndex == nodes.size())
            return false;

        const std::span<const CompiledPrefabComponent> components = nodes[nodeIndex].mComponents;
        std::size_t componentIndex = components.size();
        for (std::size_t i = 0; i < components.size(); ++i)
        {
            if (components[i].mOps->mType.name == component)
            {
                componentIndex = i;
                break;
            }
        }
        if (componentIndex == components.size())
            return false;

        const ComponentOps *ops = components[componentIndex].mOps;
        const FieldDesc *fieldDesc = ops->FindField(field);
        if (fieldDesc == nullptr || fieldDesc->mKind != FieldKind::EntityRef)
            return false;

        const std::size_t fieldIndex = static_cast<std::size_t>(fieldDesc - ops->mFields.data());
        for (const PrefabEntityRefRelocation &relocation : prefab->Relocations())
        {
            if (relocation.mNode == nodeIndex && relocation.mComponent == componentIndex &&
                relocation.mField == fieldIndex)
            {
                outTarget = nodes[relocation.mTargetNode].mAddress;
                break;
            }
        }
        return true;
    }

    StableId PlacePrefabInstance(World &world, LoadedScene &scene, const CompiledPrefab &prefab, Entity parent)
    {
        const PrefabInstantiation instantiation = InstantiatePrefab(world, prefab, parent);
        if (instantiation.mEntities.empty())
            return kNullStableId;

        ScenePrefabInstance instance;
        instance.mPrefab = ToString(prefab.Id());

        for (const CompiledPrefabNode &node : prefab.Nodes())
        {
            const auto entityIt = instantiation.mEntities.find(node.mAddress);
            if (entityIt == instantiation.mEntities.end())
                continue;

            const StableId id = AllocateStableId(scene);
            scene.mEntities[id] = entityIt->second;
            instance.mBindings[node.mAddress] = id;
            if (entityIt->second == instantiation.mRoot)
                instance.mRoot = id;
        }

        if (instance.mRoot == kNullStableId)
            return kNullStableId;

        if (!parent.IsNull())
        {
            for (const auto &[id, entity] : scene.mEntities)
            {
                if (entity == parent)
                {
                    instance.mParent = id;
                    break;
                }
            }
        }

        instance.mSiblingIndex = SiblingIndexInScene(world, instantiation.mRoot, scene);
        instance.mPlacement = InstancePlacementFromWorld(world, instantiation.mRoot);

        const StableId root = instance.mRoot;
        scene.mInstances[root] = std::move(instance);
        scene.mDirty = true;
        return root;
    }
}
