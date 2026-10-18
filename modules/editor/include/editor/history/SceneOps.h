/**
 * @file SceneOps.h
 * @author Dong Ta
 * @brief The scene calls history needs; App implements them, since the editor does not link mir::scene.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/Entity.h>

#include <cstdint>
#include <string>
#include <vector>

namespace mir
{
    struct CapturedSubtree
    {
        std::string mData;
        std::vector<Entity> mMembers; //root first, then its descendants
        std::vector<uint64_t> mStableIds; //per member, 0 for one outside the scene
    };

    class ISceneOps
    {
    public:
        virtual ~ISceneOps() = default;

        virtual Entity CreateEntity() = 0;
        virtual Entity Duplicate(Entity source) = 0;
        virtual CapturedSubtree Capture(Entity root) = 0;

        //members get their StableIds back; returned in capture order
        virtual std::vector<Entity> Restore(const CapturedSubtree &subtree, Entity parent, Entity before) = 0;

        virtual bool Destroy(Entity root) = 0;
    };
}
