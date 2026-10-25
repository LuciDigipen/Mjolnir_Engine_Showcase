/**
 * @file ContactTracker.cpp
 * @author sumin.park
 * @brief Per-pair touching state and the Begin/End events it flushes after each step.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ContactTracker.h"

#include "JoltConversions.h"

#include <Jolt/Physics/Body/Body.h>

#include <algorithm>
#include <utility>

namespace mir::detail
{
    void ContactTracker::OnContactAdded(const JPH::Body &body1, const JPH::Body &body2,
        const JPH::ContactManifold &manifold, JPH::ContactSettings &) noexcept
    {
        const JPH::SubShapeIDPair key(body1.GetID(), manifold.mSubShapeID1, body2.GetID(), manifold.mSubShapeID2);
        const std::scoped_lock lock(mMutex);
        const auto [it, created] = mPairs.try_emplace(PairKey(body1.GetID(), body2.GetID()));
        Pair &pair = it->second;
        if (created)
        {
            pair.mBodyA = std::min(body1.GetID(), body2.GetID());
            pair.mBodyB = std::max(body1.GetID(), body2.GetID());
        }
        if (mSubShapes.insert(key).second)
        {
            ++pair.mSubShapes;
            Queue(it->first, pair);
        }
        Record(pair, body1.GetID(), manifold);
    }

    void ContactTracker::OnContactPersisted(const JPH::Body &body1, const JPH::Body &body2,
        const JPH::ContactManifold &manifold, JPH::ContactSettings &) noexcept
    {
        const std::scoped_lock lock(mMutex);
        if (const auto it = mPairs.find(PairKey(body1.GetID(), body2.GetID())); it != mPairs.end())
            Record(it->second, body1.GetID(), manifold);
    }

    void ContactTracker::OnContactRemoved(const JPH::SubShapeIDPair &key) noexcept
    {
        const std::scoped_lock lock(mMutex);
        // Already ended by ForgetBody.
        if (mSubShapes.erase(key) == 0)
            return;
        if (const auto it = mPairs.find(PairKey(key.GetBody1ID(), key.GetBody2ID())); it != mPairs.end())
        {
            --it->second.mSubShapes;
            Queue(it->first, it->second);
        }
    }

    void ContactTracker::Flush(const JPH::BodyInterface &bodies, std::vector<CollisionEvent> &out)
    {
        const std::scoped_lock lock(mMutex);
        std::vector<uint64_t> queued;
        queued.swap(mQueued);
        // Sorted: callback order depends on thread timing.
        std::sort(queued.begin(), queued.end());
        for (const uint64_t key : queued)
        {
            const auto it = mPairs.find(key);
            if (it == mPairs.end())
                continue;

            Pair &pair = it->second;
            pair.mQueued = false;
            if (pair.mSubShapes > 0)
            {
                if (!pair.mTouching)
                    out.push_back(MakeEvent(CollisionPhase::Begin, pair, bodies));
                pair.mTouching = true;
                continue;
            }

            // Jolt drops a sleeping body's contacts; the pair stays touching until a body wakes.
            if (pair.mTouching && !bodies.IsActive(pair.mBodyA) && !bodies.IsActive(pair.mBodyB))
            {
                Queue(key, pair);
                continue;
            }

            // Added and removed within one Update: still an impact.
            if (!pair.mTouching)
                out.push_back(MakeEvent(CollisionPhase::Begin, pair, bodies));
            out.push_back(MakeEvent(CollisionPhase::End, pair, bodies));
            mPairs.erase(it);
        }
    }

    void ContactTracker::AppendStay(const JPH::BodyInterface &bodies, const std::unordered_set<uint32_t> &stayBodies,
        std::vector<CollisionEvent> &out)
    {
        const std::scoped_lock lock(mMutex);

        std::unordered_set<uint64_t> changed;
        for (const CollisionEvent &event : out)
            changed.insert(PairKey(JPH::BodyID(event.mBodyA.mId), JPH::BodyID(event.mBodyB.mId)));

        std::vector<uint64_t> staying;
        for (const auto &[key, pair] : mPairs)
        {
            if (!pair.mTouching || changed.contains(key))
                continue;
            if (stayBodies.contains(pair.mBodyA.GetIndexAndSequenceNumber())
                || stayBodies.contains(pair.mBodyB.GetIndexAndSequenceNumber()))
                staying.push_back(key);
        }

        // Sorted: map order is not stable.
        std::sort(staying.begin(), staying.end());
        for (const uint64_t key : staying)
            out.push_back(MakeEvent(CollisionPhase::Stay, mPairs.at(key), bodies));
    }

    void ContactTracker::ForgetBody(JPH::BodyID body, const JPH::BodyInterface &bodies,
        std::vector<CollisionEvent> &out)
    {
        const std::scoped_lock lock(mMutex);
        std::vector<uint64_t> ended;
        for (const auto &[key, pair] : mPairs)
        {
            if (pair.mBodyA == body || pair.mBodyB == body)
                ended.push_back(key);
        }
        std::sort(ended.begin(), ended.end());
        for (const uint64_t key : ended)
        {
            const auto it = mPairs.find(key);
            if (it->second.mTouching)
                out.push_back(MakeEvent(CollisionPhase::End, it->second, bodies));
            mPairs.erase(it);
        }
        std::erase_if(mSubShapes,
            [body](const JPH::SubShapeIDPair &key)
            {
                return key.GetBody1ID() == body || key.GetBody2ID() == body;
            });
    }

    uint64_t ContactTracker::PairKey(JPH::BodyID a, JPH::BodyID b)
    {
        if (b < a)
            std::swap(a, b);
        return (uint64_t(a.GetIndexAndSequenceNumber()) << 32) | b.GetIndexAndSequenceNumber();
    }

    void ContactTracker::Queue(uint64_t key, Pair &pair)
    {
        if (!pair.mQueued)
            mQueued.push_back(key);
        pair.mQueued = true;
    }

    // Keeps the latest contact for End, which Jolt reports without one.
    void ContactTracker::Record(Pair &pair, JPH::BodyID body1, const JPH::ContactManifold &manifold)
    {
        const JPH::uint count = manifold.mRelativeContactPointsOn1.size();
        if (count == 0)
            return;

        // Midway between the two surfaces.
        JPH::Vec3 sum = JPH::Vec3::sZero();
        for (JPH::uint i = 0; i < count; ++i)
            sum += manifold.mRelativeContactPointsOn1[i] + manifold.mRelativeContactPointsOn2[i];
        pair.mPoint = manifold.mBaseOffset + sum / float(2 * count);
        // Jolt's normal points from body 1 toward body 2.
        pair.mNormal = body1 == pair.mBodyA ? manifold.mWorldSpaceNormal : -manifold.mWorldSpaceNormal;
    }

    CollisionEvent ContactTracker::MakeEvent(CollisionPhase phase, const Pair &pair, const JPH::BodyInterface &bodies)
    {
        CollisionEvent event;
        event.mPhase = phase;
        event.mBodyA = BodyHandle{ pair.mBodyA.GetIndexAndSequenceNumber() };
        event.mBodyB = BodyHandle{ pair.mBodyB.GetIndexAndSequenceNumber() };
        event.mEntityA = EntityFromUserData(bodies.GetUserData(pair.mBodyA));
        event.mEntityB = EntityFromUserData(bodies.GetUserData(pair.mBodyB));
        event.mPoint = PositionFromJolt(pair.mPoint).value_or(WorldPosition {});
        event.mNormal = FromJolt(pair.mNormal);
        return event;
    }
}
