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
#include "core/RegistrationList.h"
#include "System.h"
#include "TypeId.h"
#include "World.h"
#include "core/log/Assert.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mir
{
    class TransformPropagateSystem;

    // a stage or frame-slot function
    using StageFunction = std::function<void(SystemContext &)>;

    // How systems with the same order in one phase are arranged. Reverse and Shuffle
    // are test modes, to catch a system that relies on a tie
    struct SystemTies
    {
        enum Mode : uint8_t
        {
            Registration,
            Reverse,
            Shuffle,
        };

        Mode mode = Registration;
        uint32_t seed = 0;
    };

    // MIR_SYSTEM_TIES=reverse, shuffle or shuffle:<seed>, read once. A shuffle logs its seed
    SystemTies SystemTiesFromEnvironment();

    class SystemScheduler
    {
    public:
        SystemScheduler() : SystemScheduler(SystemTiesFromEnvironment()) {}
        explicit SystemScheduler(SystemTies ties) : mTies(ties), mShuffleState(ties.seed) {}

        SystemScheduler(const SystemScheduler &) = delete;
        SystemScheduler &operator=(const SystemScheduler &) = delete;
        SystemScheduler(SystemScheduler &&) = delete;
        SystemScheduler &operator=(SystemScheduler &&) = delete;

        // Cannot register system after Start
        template <typename S, typename... Args>
        S &AddSystem(SystemPhase phase, SystemOrder order, Args &&...args)
        {
            static_assert(std::is_base_of_v<ISystem, S>, "SystemScheduler::Add: S must derive from ISystem");
            MIR_ASSERT(!mStarted, "SystemScheduler::Add: systems must be registered before Start");
            MIR_ASSERT((phase != SystemPhase::Propagate || std::is_same_v<S, TransformPropagateSystem>),
                "SystemScheduler::Add: only TransformPropagateSystem runs in Propagate");
            MIR_ASSERT(order.mValue >= SystemOrder::kMin && order.mValue <= SystemOrder::kMax,
                "SystemScheduler::Add: order {} is outside [{}, {}]", order.mValue, SystemOrder::kMin,
                SystemOrder::kMax);

            auto system = std::make_unique<S>(std::forward<Args>(args)...);
            S &ref = *system;

            // somewhere among the entries of the same order, as the tie mode picks
            std::vector<Entry> &entries = mPhases[Index(phase)];
            const auto first = std::lower_bound(entries.begin(), entries.end(), order.mValue,
                [](const Entry &entry, int value) { return entry.order < value; });
            const auto last = std::upper_bound(first, entries.end(), order.mValue,
                [](int value, const Entry &entry) { return value < entry.order; });
            entries.insert(first + TieSlot(last - first), Entry{ order.mValue, TrimTypeName<S>(), std::move(system) });
            return ref;
        }

        void Start(SystemContext &context)
        {
            MIR_ASSERT(!mStarted, "SystemScheduler::Start: already started");
            mStarted = true;
            LogOrder();

            for (std::size_t phase = 0; phase < kPhaseCount; ++phase)
            {
                for (const Entry &entry : mPhases[phase])
                    entry.system->OnStart(context);

                context.commands.Flush(context.world);
            }
        }

        // call systems in the order of phase -> SystemOrder
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

        // One tick of the resource preparation systems alone, in phase and order, for a scene that is waiting for
        // its resources. Gameplay systems do not run and need not have started.
        void UpdateLoading(SystemContext &context)
        {
            for (std::size_t phase = 0; phase < kPhaseCount; ++phase)
            {
                for (const Entry &entry : mPhases[phase])
                    entry.system->OnLoadingUpdate(context);
                context.commands.Flush(context.world);
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
                for (const Entry &entry : mPhases[phase])
                    entry.system->OnStop(context);

                context.commands.Flush(context.world);
            }

            mStarted = false;
        }

        // the function runs on every RunStage(stage) until the token dies
        [[nodiscard]] Registration AddStage(Stage stage, StageFunction function)
        {
            return mStages[static_cast<std::size_t>(stage)].Add(std::move(function));
        }

        // registration order, then flush. A throw skips the rest and the flush
        void RunStage(Stage stage, SystemContext &context)
        {
            mStages[static_cast<std::size_t>(stage)].Run(context);
            context.commands.Flush(context.world);
        }

        [[nodiscard]] Registration AddFrameSlot(FrameSlot slot, StageFunction function)
        {
            return mFrameSlots[static_cast<std::size_t>(slot)].Add(std::move(function));
        }

        void RunFrameSlot(FrameSlot slot, SystemContext &context)
        {
            mFrameSlots[static_cast<std::size_t>(slot)].Run(context);
            context.commands.Flush(context.world);
        }

        std::size_t FunctionCount(Stage stage) const { return mStages[static_cast<std::size_t>(stage)].Size(); }
        std::size_t FunctionCount(FrameSlot slot) const { return mFrameSlots[static_cast<std::size_t>(slot)].Size(); }

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

        // type names without namespaces, in run order
        std::vector<std::string_view> SystemNames(SystemPhase phase) const
        {
            std::vector<std::string_view> names;
            for (const Entry &entry : mPhases[Index(phase)])
                names.push_back(entry.name);
            return names;
        }

    private:
        static constexpr std::size_t kPhaseCount = static_cast<std::size_t>(SystemPhase::kCount);

        static constexpr std::size_t Index(SystemPhase phase) { return static_cast<std::size_t>(phase); }

        // where a new system goes among the ties already registered, 0 to ties
        std::ptrdiff_t TieSlot(std::ptrdiff_t ties);

        // Debug only
        void LogOrder() const;

        // one pass over a phase, then flush
        void RunPhase(std::size_t phase, SystemContext &context)
        {
            for (const Entry &entry : mPhases[phase])
                entry.system->OnUpdate(context);

            context.commands.Flush(context.world);
        }

        static constexpr std::size_t kStageCount = static_cast<std::size_t>(Stage::kCount);
        static constexpr std::size_t kFrameSlotCount = static_cast<std::size_t>(FrameSlot::kCount);

        struct Entry
        {
            int order;
            std::string_view name;
            std::unique_ptr<ISystem> system;
        };

        // each phase sorted by order
        std::vector<Entry> mPhases[kPhaseCount];
        RegistrationList<StageFunction> mStages[kStageCount];
        RegistrationList<StageFunction> mFrameSlots[kFrameSlotCount];
        SystemTies mTies;
        uint64_t mShuffleState;
        bool mStarted = false;
    };
}
