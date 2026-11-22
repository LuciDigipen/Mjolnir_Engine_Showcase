/**
 * @file System.h
 * @author Sumin Park
 * @brief System interface, execution phases, and the per-frame context
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#pragma once
#include "SceneInstance.h"
#include <cstdint>

namespace mir
{
    class World;
    class CommandBuffer;

    // Ordering primitive:
    // Each system will execute in the order of assigned phase -> SystemOrder
    enum class SystemPhase : uint8_t
    {
        PreUpdate,
        Update,
        // Once per fixed substep, 0..N times a frame; context.dt is the fixed step.
        // After Update so physics writes Transform last, before PostUpdate propagates it.
        FixedUpdate,
        // Transform writers that run after physics, e.g. interpolation
        PostUpdate,
        // TransformPropagateSystem alone, so every later phase reads this frame's WorldTransform
        Propagate,
        // WorldTransform readers
        LateUpdate,
        Render,
        kCount
    };

    // Where a system runs inside its phase, lowest first. Equal orders are unspecified,
    // so two systems that depend on each other need different orders.
    // Between the named borders any int in [kMin, kMax] works, e.g. SystemOrder{SystemOrder::kEarly + 5}.
    struct SystemOrder
    {
        static constexpr int kMin = 0;
        static constexpr int kEarly = 10;
        static constexpr int kNormal = 50;
        static constexpr int kLate = 90;
        static constexpr int kMax = 100;

        int mValue;
    };

    // Transitions App runs; each runs the functions modules registered for it, in
    // registration order. A SceneLoad function must not assume SceneUnload ran first
    enum class Stage : uint8_t
    {
        SceneEnd,
        SceneUnload,
        // after every SceneUnload function has let go of what it held
        SceneUnloaded,
        SceneLoad,
        SceneSaved,
        PlayBegin,
        PlayEnd,
        // a function vetoes the reload by throwing
        ReloadPrepare,
        ReloadCommit,
        // after a veto: every holder drops what it staged, the vetoer's too
        ReloadAbort,
        kCount
    };

    // Host work once per rendered frame, outside the phases
    enum class FrameSlot : uint8_t
    {
        // after requests are applied, before the UI and the systems
        FrameBegin,
        kCount
    };

    // Everything a system is handed for one tick.
    struct SystemContext
    {
        World &world;
        CommandBuffer &commands;
        float dt = 0.0f;      // seconds since the previous frame; the fixed step in FixedUpdate
        double elapsed = 0.0; // seconds since the first frame
        uint64_t frame = 0;

        float fixedAlpha = 1.0f;
        SceneInstanceId sceneInstance = kOwnedBySession;
        bool sceneSwapped = false;
    };

    // A unit of per-frame work over the World.
    class ISystem
    {
    public:
        virtual ~ISystem() = default;

        ISystem(const ISystem &) = delete;
        ISystem &operator=(const ISystem &) = delete;
        ISystem(ISystem &&) = delete;
        ISystem &operator=(ISystem &&) = delete;

        // Once, before the first OnUpdate. Cache queries here.
        virtual void OnStart(SystemContext &) {}

        // Called on target phase
        virtual void OnUpdate(SystemContext &context) = 0;

        // Once, at teardown. Runs in the same order as OnStart.
        virtual void OnStop(SystemContext &) {}

        // Runs before OnStart, resource preparation only, no gameplay.
        virtual void OnLoadingUpdate(SystemContext &) {}

    protected:
        ISystem() = default;
    };
}
