/**
 * @file CollisionLayers.h
 * @author sumin.park
 * @brief Which bodies Jolt lets collide: object layers, broad-phase layers and the contact restriction.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

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
    // Two layers is the minimum that still lets the broad phase skip the
    // static-vs-static half of the world.
    namespace Layers
    {
        constexpr JPH::ObjectLayer kNonMoving = 0;
        constexpr JPH::ObjectLayer kMoving = 1;
        constexpr JPH::ObjectLayer kCount = 2;
    }

    namespace BroadPhaseLayers
    {
        constexpr JPH::BroadPhaseLayer kNonMoving(0);
        constexpr JPH::BroadPhaseLayer kMoving(1);
        constexpr JPH::uint kCount = 2;
    }

    // Statics never collide with statics; every other pair does.
    class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter
    {
    public:
        bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
        {
            return a == Layers::kMoving || b == Layers::kMoving;
        }
    };

    class BroadPhaseLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
    {
    public:
        BroadPhaseLayerInterfaceImpl()
        {
            mLayers[Layers::kNonMoving] = BroadPhaseLayers::kNonMoving;
            mLayers[Layers::kMoving] = BroadPhaseLayers::kMoving;
        }

        JPH::uint GetNumBroadPhaseLayers() const override
        {
            return BroadPhaseLayers::kCount;
        }

        JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
        {
            JPH_ASSERT(layer < Layers::kCount);
            return mLayers[layer];
        }

    #if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
        {
            return layer == BroadPhaseLayers::kMoving ? "MOVING" : "NON_MOVING";
        }
    #endif

    private:
        JPH::BroadPhaseLayer mLayers[Layers::kCount];
    };

    class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter
    {
    public:
        bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhase) const override
        {
            return layer == Layers::kMoving || broadPhase == BroadPhaseLayers::kMoving;
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
