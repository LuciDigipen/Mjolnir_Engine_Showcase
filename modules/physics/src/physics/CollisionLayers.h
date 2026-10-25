/**
 * @file CollisionLayers.h
 * @author sumin.park
 * @brief Which bodies Jolt lets collide: object layers, broad-phase layers and the contact restriction.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <physics/PhysicsLayers.h>

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace mir::detail
{
    // An object layer is a user layer and whether the body moves, so the broad phase can still skip the
    // static-vs-static half of the world with just two broad-phase layers.
    namespace Layers
    {
        constexpr JPH::ObjectLayer kCount = kPhysicsLayerCount * 2;

        constexpr JPH::ObjectLayer Of(int userLayer, bool moving)
        {
            return static_cast<JPH::ObjectLayer>(userLayer * 2 + (moving ? 1 : 0));
        }

        constexpr bool IsMoving(JPH::ObjectLayer layer) { return (layer & 1) != 0; }
        constexpr int UserLayer(JPH::ObjectLayer layer) { return layer >> 1; }
    }

    namespace BroadPhaseLayers
    {
        constexpr JPH::BroadPhaseLayer kNonMoving(0);
        constexpr JPH::BroadPhaseLayer kMoving(1);
        constexpr JPH::uint kCount = 2;
    }

    // Statics never collide with statics; any other pair collides when the layer table says its layers do.
    // The table is read by Jolt's jobs during a step and changed only between steps.
    class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter
    {
    public:
        explicit ObjectLayerPairFilterImpl(const PhysicsLayerTable &table) : mTable(table) {}

        bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
        {
            return (Layers::IsMoving(a) || Layers::IsMoving(b)) && mTable.Collide(Layers::UserLayer(a), Layers::UserLayer(b));
        }

    private:
        const PhysicsLayerTable &mTable;
    };

    class BroadPhaseLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
    {
    public:
        JPH::uint GetNumBroadPhaseLayers() const override
        {
            return BroadPhaseLayers::kCount;
        }

        JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
        {
            JPH_ASSERT(layer < Layers::kCount);
            return Layers::IsMoving(layer) ? BroadPhaseLayers::kMoving : BroadPhaseLayers::kNonMoving;
        }

    #if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
        {
            return layer == BroadPhaseLayers::kMoving ? "MOVING" : "NON_MOVING";
        }
    #endif

    };

    class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter
    {
    public:
        bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhase) const override
        {
            return Layers::IsMoving(layer) || broadPhase == BroadPhaseLayers::kMoving;
        }
    };

    constexpr JPH::CollisionGroup::GroupID kRestrictedGroup = 1;
    constexpr JPH::CollisionGroup::GroupID kDynamicGroup = 2;

    // Read by Jolt's jobs during a step, changed only between steps.
    class ContactRestriction final : public JPH::GroupFilter
    {
    public:
        bool CanCollide(const JPH::CollisionGroup &a, const JPH::CollisionGroup &b) const override
        {
            const JPH::CollisionGroup &restricted = a.GetGroupID() == kRestrictedGroup ? a : b;
            const JPH::CollisionGroup &other = &restricted == &a ? b : a;
            if (restricted.GetGroupID() != kRestrictedGroup || other.GetGroupID() != kDynamicGroup)
                return true;
            const auto found = mAllowed.find(other.GetSubGroupID());
            return found == mAllowed.end() ||
                std::binary_search(found->second.begin(), found->second.end(), restricted.GetSubGroupID());
        }

        // By body index, sorted.
        std::unordered_map<JPH::uint32, std::vector<JPH::uint32>> mAllowed;
    };
}
