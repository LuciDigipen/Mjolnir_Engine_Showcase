/**
 * @file ActionScheduler.cpp
 * @author Quy Dong Ta
 * @brief World resource that eases entity transforms and named values over time.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <actions/ActionScheduler.h>

#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>
#include <core/log/Log.h>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace mir
{
    namespace
    {
        
        template <glm::length_t N, typename T>
        bool IsFinite(const glm::vec<N, T> &v)
        {
            for (glm::length_t i = 0; i < N; ++i)
                if (!std::isfinite(v[i]))
                    return false;
            return true;
        }


        template <typename T>
        T Blend(T from, T to, T t)
        {
            if (t == T(0))
                return from;
            if (t == T(1))
                return to;
            const T value = from * (T(1) - t) + to * t;
            if (std::isfinite(value))
                return value;
            
            return t < T(0.5) ? from : to;
        }

        template <glm::length_t N, typename T>
        glm::vec<N, T> Blend(const glm::vec<N, T> &from, const glm::vec<N, T> &to, float t)
        {
            glm::vec<N, T> out;
            for (glm::length_t i = 0; i < N; ++i)
                out[i] = Blend<T>(from[i], to[i], static_cast<T>(t));
            return out;
        }

        bool ValidTiming(const ActionParams &params, std::string_view call)
        {
            if (!std::isfinite(params.mDuration) || params.mDuration < 0.0f)
            {
                MIR_LOG_ERROR("ActionScheduler::{}: duration must be a finite number from 0, got {}", call, params.mDuration);
                return false;
            }
            if (!std::isfinite(params.mDelay) || params.mDelay < 0.0f)
            {
                MIR_LOG_ERROR("ActionScheduler::{}: delay must be a finite number from 0, got {}", call, params.mDelay);
                return false;
            }
            return true;
        }

        bool ValidScale(float scale, std::string_view call)
        {
            if (std::isfinite(scale) && scale >= 0.0f)
                return true;
            MIR_LOG_ERROR("ActionScheduler::{}: the time scale must be a finite number from 0, got {}", call, scale);
            return false;
        }
    }

    ActionHandle ActionScheduler::MoveTo(Entity target, const WorldVec3 &goal, const ActionParams &params)
    {
        if (target.IsNull() || !IsFinite(goal))
        {
            MIR_LOG_ERROR("ActionScheduler::MoveTo: needs an entity and a finite goal");
            return {};
        }
        Action action;
        action.mKind = Kind::Move;
        action.mTarget = target;
        action.mLane = ActionLane(target);
        action.mToPosition = goal;
        return Add(action, params, "MoveTo");
    }

    ActionHandle ActionScheduler::RotateTo(Entity target, const glm::quat &goal, const ActionParams &params)
    {
        const float length = glm::length(goal);
        if (target.IsNull() || !std::isfinite(length) || !(length > 0.0f))
        {
            MIR_LOG_ERROR("ActionScheduler::RotateTo: needs an entity and a finite rotation that is not all zeros");
            return {};
        }
        Action action;
        action.mKind = Kind::Rotate;
        action.mTarget = target;
        action.mLane = ActionLane(target);
        action.mToRotation = goal / length;
        return Add(action, params, "RotateTo");
    }

    ActionHandle ActionScheduler::ScaleTo(Entity target, const glm::vec3 &goal, const ActionParams &params)
    {
        if (target.IsNull() || !IsFinite(goal))
        {
            MIR_LOG_ERROR("ActionScheduler::ScaleTo: needs an entity and a finite goal");
            return {};
        }
        Action action;
        action.mKind = Kind::Scale;
        action.mTarget = target;
        action.mLane = ActionLane(target);
        action.mTo = glm::vec4(goal, 0.0f);
        return Add(action, params, "ScaleTo");
    }

    ActionHandle ActionScheduler::TweenValue(std::string_view key, float goal, const ActionParams &params)
    {
        return AddValue(key, glm::vec4(goal, 0.0f, 0.0f, 0.0f), 1, params, "TweenValue");
    }

    ActionHandle ActionScheduler::TweenVec4(std::string_view key, const glm::vec4 &goal, const ActionParams &params)
    {
        return AddValue(key, goal, 4, params, "TweenVec4");
    }

    ActionHandle ActionScheduler::AddValue(std::string_view key, const glm::vec4 &goal, uint8_t components,
        const ActionParams &params, std::string_view call)
    {
        if (key.empty() || !IsFinite(goal))
        {
            MIR_LOG_ERROR("ActionScheduler::{}: needs a key and a finite goal", call);
            return {};
        }
        Action action;
        action.mKind = Kind::Value;
        action.mKey = ActionLane(key);
        action.mLane = action.mKey;
        action.mComponents = components;
        action.mTo = goal;
        return Add(action, params, call);
    }

    ActionHandle ActionScheduler::Add(Action action, const ActionParams &params, std::string_view call)
    {
        if (!ValidTiming(params, call))
            return {};

        action.mId = mNextId;
        mNextId = mNextId == std::numeric_limits<uint32_t>::max() ? 1 : mNextId + 1;
        action.mClock = params.mClock;
        action.mEase = params.mEase;
        action.mBlocking = params.mBlocking;
        action.mDuration = params.mDuration;
        action.mDelay = params.mDelay;
        if (params.mLane != 0)
            action.mLane = params.mLane;

        mActions.push_back(action);
        return ActionHandle{ action.mId };
    }

    bool ActionScheduler::SetValue(std::string_view key, float value)
    {
        if (key.empty() || !std::isfinite(value))
        {
            MIR_LOG_ERROR("ActionScheduler::SetValue: needs a key and a finite value");
            return false;
        }
        mValues[ActionLane(key)].x = value;
        return true;
    }

    bool ActionScheduler::SetVec4(std::string_view key, const glm::vec4 &value)
    {
        if (key.empty() || !IsFinite(value))
        {
            MIR_LOG_ERROR("ActionScheduler::SetVec4: needs a key and a finite value");
            return false;
        }
        mValues[ActionLane(key)] = value;
        return true;
    }

    float ActionScheduler::Value(std::string_view key, float fallback) const
    {
        const auto found = mValues.find(ActionLane(key));
        return found != mValues.end() ? found->second.x : fallback;
    }

    glm::vec4 ActionScheduler::Vec4(std::string_view key, const glm::vec4 &fallback) const
    {
        const auto found = mValues.find(ActionLane(key));
        return found != mValues.end() ? found->second : fallback;
    }

    bool ActionScheduler::HasValue(std::string_view key) const { return mValues.contains(ActionLane(key)); }

    bool ActionScheduler::IsRunning(ActionHandle handle) const
    {
        return handle.IsValid() &&
               std::any_of(mActions.begin(), mActions.end(), [&](const Action &a) { return a.mId == handle.mId && !a.mDone; });
    }

    bool ActionScheduler::IsLaneBusy(uint64_t lane) const
    {
        return std::any_of(mActions.begin(), mActions.end(), [&](const Action &a) { return a.mLane == lane && !a.mDone; });
    }

    void ActionScheduler::Cancel(ActionHandle handle)
    {
        std::erase_if(mActions, [&](const Action &a) { return a.mId == handle.mId; });
    }

    void ActionScheduler::CancelAll(Entity target)
    {
        std::erase_if(mActions, [&](const Action &a) { return a.mKind != Kind::Value && a.mTarget == target; });
    }

    void ActionScheduler::CancelValue(std::string_view key)
    {
        const uint64_t hash = ActionLane(key);
        std::erase_if(mActions, [&](const Action &a) { return a.mKind == Kind::Value && a.mKey == hash; });
    }

    void ActionScheduler::CancelLane(uint64_t lane)
    {
        std::erase_if(mActions, [&](const Action &a) { return a.mLane == lane; });
    }

    void ActionScheduler::Clear()
    {
        mActions.clear();
        mValues.clear();
        mLanes.clear();
        mTimeScales = { 1.0f, 1.0f };
    }

    bool ActionScheduler::SetTimeScale(ActionClock clock, float scale)
    {
        if (!ValidScale(scale, "SetTimeScale"))
            return false;
        mTimeScales[Index(clock)] = scale;
        return true;
    }

    void ActionScheduler::SetLanePaused(uint64_t lane, bool paused)
    {
        LaneSettings &settings = mLanes[lane];
        settings.mPaused = paused;
        if (!settings.mPaused && settings.mTimeScale == 1.0f)
            mLanes.erase(lane);
    }

    bool ActionScheduler::IsLanePaused(uint64_t lane) const
    {
        const auto found = mLanes.find(lane);
        return found != mLanes.end() && found->second.mPaused;
    }

    bool ActionScheduler::SetLaneTimeScale(uint64_t lane, float scale)
    {
        if (!ValidScale(scale, "SetLaneTimeScale"))
            return false;
        LaneSettings &settings = mLanes[lane];
        settings.mTimeScale = scale;
        if (!settings.mPaused && settings.mTimeScale == 1.0f)
            mLanes.erase(lane);
        return true;
    }

    float ActionScheduler::LaneTimeScale(uint64_t lane) const
    {
        const auto found = mLanes.find(lane);
        return found != mLanes.end() ? found->second.mTimeScale : 1.0f;
    }

    void ActionScheduler::Tick(World &world, ActionClock clock, float dt)
    {
        if (!std::isfinite(dt) || dt < 0.0f)
            return;


        const float clockScale = TimeScale(clock);
        if (clockScale == 0.0f)
            return;

        const float clockDt = dt * clockScale;
        mBlockedLanes.clear();
        for (Action &action : mActions)
        {
            if (action.mClock != clock || action.mDone || IsLaneBlocked(action.mLane))
                continue;

            const auto lane = mLanes.find(action.mLane);
            const float laneScale = lane != mLanes.end() ? lane->second.mTimeScale : 1.0f;
            if (lane != mLanes.end() && (lane->second.mPaused || laneScale == 0.0f))
                continue;

            action.mDone = Advance(world, action, clockDt * laneScale);
            if (!action.mDone && action.mBlocking)
                mBlockedLanes.push_back(action.mLane);
        }
        std::erase_if(mActions, [](const Action &a) { return a.mDone; });
    }

    bool ActionScheduler::IsLaneBlocked(uint64_t lane) const
    {
        return std::find(mBlockedLanes.begin(), mBlockedLanes.end(), lane) != mBlockedLanes.end();
    }

    // true when the action is finished or can no longer run
    bool ActionScheduler::Advance(World &world, Action &action, float dt)
    {
        
        if (action.mKind != Kind::Value && !world.IsAlive(action.mTarget))
            return true;

        if (action.mDelay > 0.0f)
        {
            action.mDelay -= dt;
            if (action.mDelay > 0.0f)
                return false;
            dt = -action.mDelay;
            action.mDelay = 0.0f;
        }

        if (!action.mStarted)
        {
            if (!Start(world, action))
                return true;
            action.mStarted = true;
        }

        action.mAge += dt;
        const float t = action.mDuration > 0.0f ? std::min(action.mAge / action.mDuration, 1.0f) : 1.0f;
        if (!Apply(world, action, ApplyEase(action.mEase, t)))
            return true;
        return t >= 1.0f;
    }

    bool ActionScheduler::Start(World &world, Action &action)
    {
        if (action.mKind == Kind::Value)
        {
            const auto found = mValues.find(action.mKey);
            action.mFrom = found != mValues.end() ? found->second : glm::vec4(0.0f);
            return true;
        }

        const Transform *transform = world.GetComponent<Transform>(action.mTarget);
        if (transform == nullptr)
        {
            // a target destroyed before its turn is normal, one alive with no Transform is a mistake
            if (world.IsAlive(action.mTarget))
                MIR_LOG_WARN("ActionScheduler: entity {} has no Transform, its action is dropped", action.mTarget.mIndex);
            return false;
        }

        action.mFromPosition = transform->Position();
        action.mFromRotation = transform->Rotation();
        action.mFrom = glm::vec4(transform->Scale(), 0.0f);
        return true;
    }

    bool ActionScheduler::Apply(World &world, const Action &action, float eased)
    {
        if (action.mKind == Kind::Value)
        {
            glm::vec4 &slot = mValues[action.mKey];
            if (action.mComponents == 1)
                slot.x = Blend(action.mFrom.x, action.mTo.x, eased);
            else
                slot = Blend(action.mFrom, action.mTo, eased);
            return true;
        }

        Transform *transform = world.GetComponent<Transform>(action.mTarget);
        if (transform == nullptr)
            return false;

        switch (action.mKind)
        {
        case Kind::Move:
            transform->SetPosition(Blend(action.mFromPosition, action.mToPosition, eased));
            break;
        case Kind::Rotate:
            transform->SetRotation(eased == 1.0f ? action.mToRotation
                                                 : glm::normalize(glm::slerp(action.mFromRotation, action.mToRotation, eased)));
            break;
        case Kind::Scale:
            transform->SetScale(Blend(glm::vec3(action.mFrom), glm::vec3(action.mTo), eased));
            break;
        case Kind::Value:
            break;
        }
        return true;
    }
}
