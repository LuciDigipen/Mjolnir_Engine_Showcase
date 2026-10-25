/**
 * @file ContactTracker.h
 * @author sumin.park
 * @brief Folds Jolt's per sub-shape contacts into one Begin/End event stream per body pair.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <physics/CollisionEvents.h>

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/ContactListener.h>

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mir::detail
{
    class ContactTracker final : public JPH::ContactListener
    {
      public:
        // Called on Jolt's job threads with bodies locked: read only IDs and the manifold.
        // noexcept because an exception cannot cross Jolt or leave a job thread.
        void OnContactAdded(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                            JPH::ContactSettings &) noexcept override;
        void OnContactPersisted(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                                JPH::ContactSettings &) noexcept override;
        void OnContactRemoved(const JPH::SubShapeIDPair &key) noexcept override;

        void Flush(const JPH::BodyInterface &bodies, std::vector<CollisionEvent> &out);

        // Call before the body leaves Jolt, while its user data is still readable.
        void ForgetBody(JPH::BodyID body, const JPH::BodyInterface &bodies, std::vector<CollisionEvent> &out);

      private:
        struct Pair
        {
            // A has the lower ID.
            JPH::BodyID mBodyA;
            JPH::BodyID mBodyB;
            uint32_t mSubShapes = 0;
            // Begin sent, End not yet.
            bool mTouching = false;
            bool mQueued = false;
            JPH::RVec3 mPoint = JPH::RVec3::sZero();
            // A toward B.
            JPH::Vec3 mNormal = JPH::Vec3::sZero();
        };

        static uint64_t PairKey(JPH::BodyID a, JPH::BodyID b);
        void Queue(uint64_t key, Pair &pair);
        static void Record(Pair &pair, JPH::BodyID body1, const JPH::ContactManifold &manifold);
        static CollisionEvent MakeEvent(CollisionPhase phase, const Pair &pair, const JPH::BodyInterface &bodies);

        std::mutex mMutex;
        std::unordered_set<JPH::SubShapeIDPair> mSubShapes;
        std::unordered_map<uint64_t, Pair> mPairs;
        // Pairs whose sub-shape count changed, or that sleep in contact.
        std::vector<uint64_t> mQueued;
    };
}
