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
#include <core/math/WorldCoordinates.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    struct CapturedSubtree
    {
        std::string mData;
        std::vector<Entity> mMembers; //root first, then its descendants
        std::vector<uint64_t> mStableIds; //per member, 0 for one outside the scene
        std::vector<std::string> mInstances; //App-private: records whose root is a member
    };

    class ISceneOps
    {
    public:
        virtual ~ISceneOps() = default;

        virtual Entity CreateEntity() = 0;
        virtual Entity Duplicate(Entity source) = 0;
        virtual Entity CopyAsOrdinaryObject(Entity source) = 0;
        virtual CapturedSubtree Capture(Entity root) = 0;

        //members get their StableIds back; returned in capture order
        virtual std::vector<Entity> Restore(const CapturedSubtree &subtree, Entity parent, Entity before) = 0;

        virtual bool Destroy(Entity root) = 0;

        // A linked instance of the Prefab at contentPath, or null when it cannot be placed.
        // position is local to parent, or a world position for a root.
        virtual Entity PlacePrefab(std::string_view contentPath, Entity parent, const WorldVec3 &position) = 0;

        // False when the scene refuses to restructure the targets or the new parent.
        virtual bool CanReparent(std::span<const Entity>, Entity) { return true; }
    };
}
