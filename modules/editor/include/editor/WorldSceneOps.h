/**
 * @file WorldSceneOps.h
 * @author Dong Ta
 * @brief The scene calls the editor's history makes, done on a World and its LoadedScene.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/history/SceneOps.h>

namespace mir
{
    class World;
    struct LoadedScene;

    class WorldSceneOps final : public ISceneOps
    {
    public:
        // scene is the one the World holds; App replaces its value, never the object
        WorldSceneOps(World &world, LoadedScene &scene) : mWorld(world), mScene(scene) {}

        Entity CreateEntity() override;
        Entity Duplicate(Entity source) override;
        Entity CopyAsOrdinaryObject(Entity source) override;
        CapturedSubtree Capture(Entity root) override;
        std::vector<Entity> Restore(const CapturedSubtree &subtree, Entity parent, Entity before) override;
        bool Destroy(Entity root) override;
        bool CanReparent(std::span<const Entity> entities, Entity parent) override;

    private:
        World &mWorld;
        LoadedScene &mScene;
    };
}
