/**
 * @file ActionScheduler.h
 * @author Quy Dong Ta
 * @brief World resource that eases entity transforms and named values over time.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "Ease.h"

#include <core/ecs/Entity.h>
#include <core/math/WorldReal.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mir
{
    class World;

    // Game stops while the simulation is paused or editing. UI follows the frame, so a pause menu keeps animating
    enum class ActionClock : uint8_t
    {
        Game,
        UI,
    };

    // A named lane, never 0. A value key is its own lane, so ActionLane("Pause.alpha") is the lane a tween of
    // that value runs in when it names none
    constexpr uint64_t ActionLane(std::string_view name)
    {
        uint64_t hash = 0xcbf29ce484222325ull;
        for (const char c : name)
        {
            hash ^= static_cast<uint8_t>(c);
            hash *= 0x100000001b3ull;
        }
        return hash != 0 ? hash : 1;
    }

    // The lane an entity's actions run in when they name none
    constexpr uint64_t ActionLane(Entity entity)
    {
        uint64_t x = PackEntity(entity) + 0x9e3779b97f4a7c15ull;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
        x ^= x >> 31;
        return x != 0 ? x : 1;
    }

    struct ActionParams
    {
        float mDuration = 1.0f;
        float mDelay = 0.0f;
        Ease mEase = Ease::Linear;
        ActionClock mClock = ActionClock::Game;
        // holds back every later action in its lane, on the same clock, until it finishes
        bool mBlocking = false;
        // 0 is the target's own lane
        uint64_t mLane = 0;
    };

    struct ActionHandle
    {
        uint32_t mId = 0;

        constexpr bool IsValid() const { return mId != 0; }
        constexpr bool operator==(const ActionHandle &) const = default;
    };


    class ActionScheduler
    {
    public:
        ActionHandle MoveTo(Entity target, const WorldVec3 &goal, const ActionParams &params = {});
        ActionHandle RotateTo(Entity target, const glm::quat &goal, const ActionParams &params = {});
        ActionHandle ScaleTo(Entity target, const glm::vec3 &goal, const ActionParams &params = {});

        // Named values for what has no entity, such as game UI. A tween starts from the value the key holds
        // when its delay ends, and a key never set holds 0
        ActionHandle TweenValue(std::string_view key, float goal, const ActionParams &params = {});
        ActionHandle TweenVec4(std::string_view key, const glm::vec4 &goal, const ActionParams &params = {});
       
        bool SetValue(std::string_view key, float value);
        bool SetVec4(std::string_view key, const glm::vec4 &value);
        float Value(std::string_view key, float fallback = 0.0f) const;
        glm::vec4 Vec4(std::string_view key, const glm::vec4 &fallback = glm::vec4(0.0f)) const;
        bool HasValue(std::string_view key) const;

        bool IsRunning(ActionHandle handle) const;
        bool IsLaneBusy(uint64_t lane) const;
        std::size_t Count() const { return mActions.size(); }

        // A cancelled action leaves its target where it got to
        void Cancel(ActionHandle handle);
        void CancelAll(Entity target);
        void CancelValue(std::string_view key);
        void CancelLane(uint64_t lane);
        // every action, value, lane setting and time scale
        void Clear();

        // A scale of 0 freezes like a pause. False, logged, for a negative or non-finite scale
        bool SetTimeScale(ActionClock clock, float scale);
        float TimeScale(ActionClock clock) const { return mTimeScales[Index(clock)]; }
        void SetLanePaused(uint64_t lane, bool paused);
        bool IsLanePaused(uint64_t lane) const;
        bool SetLaneTimeScale(uint64_t lane, float scale);
        float LaneTimeScale(uint64_t lane) const;

        // Advances every action on that clock by dt seconds
        void Tick(World &world, ActionClock clock, float dt);

    private:
        enum class Kind : uint8_t
        {
            Move,
            Rotate,
            Scale,
            Value,
        };

        struct Action
        {
            uint32_t mId = 0;
            Kind mKind = Kind::Value;
            ActionClock mClock = ActionClock::Game;
            Ease mEase = Ease::Linear;
            bool mBlocking = false;
            bool mStarted = false;
            bool mDone = false;
            uint8_t mComponents = 1;
            uint64_t mLane = 0;
            Entity mTarget;
            uint64_t mKey = 0;
            float mDuration = 0.0f;
            float mDelay = 0.0f;
            float mAge = 0.0f;
            WorldVec3 mFromPosition{ 0 };
            WorldVec3 mToPosition{ 0 };
            glm::quat mFromRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
            glm::quat mToRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
            // scale in xyz, a value in x or all four
            glm::vec4 mFrom{ 0.0f };
            glm::vec4 mTo{ 0.0f };
        };

        struct LaneSettings
        {
            bool mPaused = false;
            float mTimeScale = 1.0f;
        };

        static constexpr std::size_t Index(ActionClock clock) { return static_cast<std::size_t>(clock); }

        ActionHandle Add(Action action, const ActionParams &params, std::string_view call);
        ActionHandle AddValue(std::string_view key, const glm::vec4 &goal, uint8_t components,
            const ActionParams &params, std::string_view call);
        bool Advance(World &world, Action &action, float dt);
        bool Start(World &world, Action &action);
        bool Apply(World &world, const Action &action, float eased);
        bool IsLaneBlocked(uint64_t lane) const;

        std::vector<Action> mActions;
        std::unordered_map<uint64_t, glm::vec4> mValues; // to store UI values, since GameUI does not have place to store data between frames
        std::unordered_map<uint64_t, LaneSettings> mLanes;
        std::array<float, 2> mTimeScales{ 1.0f, 1.0f };
        std::vector<uint64_t> mBlockedLanes;
        uint32_t mNextId = 1;
    };
}
