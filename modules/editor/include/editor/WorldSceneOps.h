/**
 * @file WorldSceneOps.h
 * @author Dong Ta
 * @brief The scene calls the editor's history makes, done on a World and the scenes it holds.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/history/SceneOps.h>

#include <core/ecs/SceneInstance.h>

#include <filesystem>

namespace mir
{
    class PrefabWorkspace;
    class World;
    struct LoadedScene;

    class WorldSceneOps final : public ISceneOps
    {
    public:
        // Each call works on the scene that owns its entity, found in the World's SceneCollection, so additive
        // scenes keep their own records. projectRoot resolves the content paths PlacePrefab is given.
        WorldSceneOps(World &world, PrefabWorkspace &prefabs, std::filesystem::path projectRoot)
            : mWorld(world), mPrefabs(prefabs), mProjectRoot(std::move(projectRoot))
        {
        }

        Entity CreateEntity() override;
        Entity Duplicate(Entity source) override;
        Entity CopyAsOrdinaryObject(Entity source) override;
        CapturedSubtree Capture(Entity root) override;
        std::vector<Entity> Restore(const CapturedSubtree &subtree, Entity parent, Entity before) override;
        bool Destroy(Entity root) override;
        Entity PlacePrefab(std::string_view contentPath, Entity parent, const WorldVec3 &position) override;
        bool CanReparent(std::span<const Entity> entities, Entity parent) override;

    private:
        // The scene's records, or nullptr for an entity no loaded scene owns.
        LoadedScene *FindScene(SceneInstanceId instance);

        World &mWorld;
        PrefabWorkspace &mPrefabs;
        std::filesystem::path mProjectRoot;
    };
}
