/**
 * @file AppSceneOps.h
 * @author Dong Ta
 * @brief The scene calls the editor's history makes, done on App's World and LoadedScene.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/history/SceneOps.h>

namespace mir
{
    class App;

    class AppSceneOps final : public ISceneOps
    {
    public:
        explicit AppSceneOps(App &app) : mApp(app) {}

        Entity CreateEntity() override;
        Entity Duplicate(Entity source) override;
        Entity CopyAsOrdinaryObject(Entity source) override;
        CapturedSubtree Capture(Entity root) override;
        std::vector<Entity> Restore(const CapturedSubtree &subtree, Entity parent, Entity before) override;
        bool Destroy(Entity root) override;
        bool CanReparent(std::span<const Entity> entities, Entity parent) override;

    private:
        //a copied or restored ScriptRef has no instance, and ScriptSystem skips those
        void ResolveScripts();

        App &mApp;
    };
}
