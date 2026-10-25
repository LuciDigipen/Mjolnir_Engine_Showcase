/**
 * @file SystemScheduler.h
 * @author Sumin Park
 * @brief Owns systems and runs them in phase order; holds the stage and frame-slot lists
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#pragma once
#include "CommandBuffer.h"
#include "Registration.h"
#include "System.h"
#include "World.h"
#include "core/log/Assert.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace mir
{
    class TransformPropagateSystem;

    // a stage or frame-slot function
    using StageFunction = std::function<void(SystemContext &)>;

    class SystemScheduler
    {
    public:
        SystemScheduler() = default;

        SystemScheduler(const SystemScheduler &) = delete;
        SystemScheduler &operator=(const SystemScheduler &) = delete;
        SystemScheduler(SystemScheduler &&) = delete;
        SystemScheduler &operator=(SystemScheduler &&) = delete;

        // Cannot register system after Start
        template <typename S, typename... Args>
        S &AddSystem(SystemPhase phase, Args &&...args)
        {
            static_assert(std::is_base_of_v<ISystem, S>, "SystemScheduler::Add: S must derive from ISystem");
            MIR_ASSERT(!mStarted, "SystemScheduler::Add: systems must be registered before Start");
            MIR_ASSERT((phase != SystemPhase::Propagate || std::is_same_v<S, TransformPropagateSystem>),
                "SystemScheduler::Add: only TransformPropagateSystem runs in Propagate");

            auto system = std::make_unique<S>(std::forward<Args>(args)...);
            S &ref = *system;
            mPhases[Index(phase)].push_back(std::move(system));
            return ref;
        }

        void Start(SystemContext &context)
        {
            MIR_ASSERT(!mStarted, "SystemScheduler::Start: already started");
            mStarted = true;

            for (std::size_t phase = 0; phase < kPhaseCount; ++phase)
            {
                for (const std::unique_ptr<ISystem> &system : mPhases[phase])
                    system->OnStart(context);

                context.commands.Flush(context.world);
            }
        }

        // call systems in the order of phase -> registrated order
        // buffer is flushed after every phase, and after every FixedUpdate substep
        // FixedUpdate runs fixedSteps times with dt = fixedDt; every other phase runs once with context.dt
        void Update(SystemContext &context, uint32_t fixedSteps, float fixedDt)
        {
            MIR_ASSERT(mStarted, "SystemScheduler::Update: Start was never called");

            const std::size_t fixedPhase = Index(SystemPhase::FixedUpdate);

            for (std::size_t phase = 0; phase < kPhaseCount; ++phase)
            {
                if (phase != fixedPhase)
                {
                    RunPhase(phase, context);
                    continue;
                }

                SystemContext fixedContext = context;
                fixedContext.dt = fixedDt;
                for (uint32_t step = 0; step < fixedSteps; ++step)
                    RunPhase(phase, fixedContext);
            }
        }

        // every phase once, FixedUpdate included, all with context.dt
        // for tests and callers without a fixed clock; App passes its substeps
        void Update(SystemContext &context) { Update(context, 1, context.dt); }

        // same order as Start, so scripts stop while the runtimes they call still run
        void Stop(SystemContext &context)
        {
            if (!mStarted)
                return;

            for (std::size_t phase = 0; phase < kPhaseCount; ++phase)
            {
                for (const std::unique_ptr<ISystem> &system : mPhases[phase])
                    system->OnStop(context);

                context.commands.Flush(context.world);
            }

            mStarted = false;
        }

        // the function runs on every RunStage(stage) until the token dies
        [[nodiscard]] Registration AddStage(Stage stage, StageFunction function)
        {
            return Add(mStages[static_cast<std::size_t>(stage)], std::move(function));
        }

        // registration order, then flush. A throw skips the rest and the flush
        void RunStage(Stage stage, SystemContext &context) { Run(mStages[static_cast<std::size_t>(stage)], context); }

        [[nodiscard]] Registration AddFrameSlot(FrameSlot slot, StageFunction function)
        {
            return Add(mFrameSlots[static_cast<std::size_t>(slot)], std::move(function));
        }

        void RunFrameSlot(FrameSlot slot, SystemContext &context)
        {
            Run(mFrameSlots[static_cast<std::size_t>(slot)], context);
        }

        std::size_t FunctionCount(Stage stage) const { return mStages[static_cast<std::size_t>(stage)].size(); }
        std::size_t FunctionCount(FrameSlot slot) const { return mFrameSlots[static_cast<std::size_t>(slot)].size(); }

        // Drops all registered system
        // Error if Stop wasn't called yet
        void Reset()
        {
            MIR_ASSERT(!mStarted, "SystemScheduler::Reset: Stop must run before Reset");

            for (std::size_t phase = 0; phase < kPhaseCount; ++phase)
                mPhases[phase].clear();
        }

        bool Started() const { return mStarted; }

        std::size_t SystemCount(SystemPhase phase) const { return mPhases[Index(phase)].size(); }

        std::size_t SystemCount() const
        {
            std::size_t total = 0;
            for (std::size_t phase = 0; phase < kPhaseCount; ++phase)
                total += mPhases[phase].size();
            return total;
        }

    private:
        static constexpr std::size_t kPhaseCount = static_cast<std::size_t>(SystemPhase::kCount);

        static constexpr std::size_t Index(SystemPhase phase) { return static_cast<std::size_t>(phase); }

        // one pass over a phase, then flush
        void RunPhase(std::size_t phase, SystemContext &context)
        {
            for (const std::unique_ptr<ISystem> &system : mPhases[phase])
                system->OnUpdate(context);

            context.commands.Flush(context.world);
        }

        struct Entry
        {
            uint64_t id;
            StageFunction function;
        };

        // lists change only between runs: adding or removing during one could move the
        // function that is running
        Registration Add(std::vector<Entry> &list, StageFunction function)
        {
            MIR_ASSERT(mRunning == 0, "SystemScheduler: registered from inside a stage or frame slot");
            const uint64_t id = ++mNextId;
            list.push_back(Entry{ id, std::move(function) });

            return Registration(mAlive, [this, &list, id]
                {
                    MIR_ASSERT(mRunning == 0, "SystemScheduler: unregistered from inside a stage or frame slot");
                    std::erase_if(list, [id](const Entry &entry) { return entry.id == id; });
                });
        }

        void Run(const std::vector<Entry> &list, SystemContext &context)
        {
            const Running running{ mRunning };
            for (const Entry &entry : list)
                entry.function(context);

            context.commands.Flush(context.world);
        }

        // RAII, so a throwing function still lowers it
        struct Running
        {
            uint32_t &depth;

            explicit Running(uint32_t &value) : depth(value) { ++depth; }
            ~Running() { --depth; }
        };

        static constexpr std::size_t kStageCount = static_cast<std::size_t>(Stage::kCount);
        static constexpr std::size_t kFrameSlotCount = static_cast<std::size_t>(FrameSlot::kCount);

        std::vector<std::unique_ptr<ISystem>> mPhases[kPhaseCount];
        std::vector<Entry> mStages[kStageCount];
        std::vector<Entry> mFrameSlots[kFrameSlotCount];
        uint64_t mNextId = 0;
        uint32_t mRunning = 0;
        bool mStarted = false;

        // tokens hold it weakly, to assert the scheduler outlives them
        std::shared_ptr<const int> mAlive = std::make_shared<const int>(0);
    };
}
