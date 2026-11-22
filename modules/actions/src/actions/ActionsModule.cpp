/**
 * @file ActionsModule.cpp
 * @author Quy Dong Ta
 * @brief Owns the ActionScheduler resource, its system and its clocks.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <actions/ActionsModule.h>

#include <actions/ActionScheduler.h>
#include <actions/ActionSystem.h>

#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>

namespace mir
{
    ActionsModule::ActionsModule(ModuleContext context)
        : mScheduler(context.world.EmplaceResource<ActionScheduler>())
    {
        context.scheduler.AddSystem<ActionSystem>(SystemPhase::Update, SystemOrder{ SystemOrder::kNormal });

        mUIClock = context.scheduler.AddFrameSlot(FrameSlot::FrameBegin,
            [this](SystemContext &frame) { mScheduler.Tick(frame.world, ActionClock::UI, frame.dt); });
        mPlayEnd = context.scheduler.AddStage(Stage::PlayEnd, [this](SystemContext &) { mScheduler.Clear(); });
    }
}
