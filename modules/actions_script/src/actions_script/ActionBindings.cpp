/**
 * @file ActionBindings.cpp
 * @author Quy Dong Ta
 * @brief Extends the World usertype with world:move_to, world:tween_value and the other action calls.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <actions_script/ActionBindings.h>

#include <script/BindingApi.h>
#include <script/BindingHelpers.h>

#include <actions/ActionScheduler.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>

#include <sol/sol.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace mir
{
    namespace
    {
        using LuaHandle = sol::optional<uint32_t>;

        bool Given(const sol::optional<sol::object> &argument)
        {
            return argument.has_value() && argument->valid() && argument->get_type() != sol::type::nil;
        }

        ActionScheduler *SchedulerOf(World &world, std::string_view call)
        {
            ActionScheduler *actions = world.TryResource<ActionScheduler>();
            if (actions == nullptr)
                MIR_LOG_ERROR("script: world:{}: this world has no ActionScheduler", call);
            return actions;
        }

        LuaHandle ToLua(ActionHandle handle)
        {
            if (!handle.IsValid())
                return sol::nullopt;
            return handle.mId;
        }

        std::optional<ActionHandle> ReadHandle(const sol::object &value, std::string_view call)
        {
            if (value.get_type() != sol::type::number)
            {
                MIR_LOG_ERROR("script: world:{}: handle must be the number an action call returned", call);
                return std::nullopt;
            }
            const double number = value.as<double>();
            if (!(number >= 1.0) || number > std::numeric_limits<uint32_t>::max() || std::floor(number) != number)
            {
                MIR_LOG_ERROR("script: world:{}: {} is not an action handle", call, number);
                return std::nullopt;
            }
            return ActionHandle{ static_cast<uint32_t>(number) };
        }

        // A lane is a name, or an entity for that entity's own lane
        std::optional<uint64_t> ReadLane(const sol::object &value, std::string_view call, std::string_view argument)
        {
            if (value.is<Entity>())
                return ActionLane(value.as<Entity>());
            if (value.get_type() == sol::type::string)
            {
                const std::string name = value.as<std::string>();
                if (!name.empty())
                    return ActionLane(name);
            }
            MIR_LOG_ERROR("script: world:{}: {} must be a lane name or an entity", call, argument);
            return std::nullopt;
        }

        bool ReadSeconds(const sol::object &value, float &out, std::string_view call, std::string_view argument)
        {
            double number = 0.0;
            if (!ReadNumber(value, number, call, argument))
                return false;
            if (number < 0.0)
            {
                MIR_LOG_ERROR("script: {}: {} must not be negative", call, argument);
                return false;
            }
            out = static_cast<float>(number);
            return true;
        }

        // False, logged, for anything it does not understand, so a typo never quietly runs with defaults
        bool ReadOptions(const sol::optional<sol::object> &options, ActionParams &params, std::string_view name)
        {
            if (!Given(options))
                return true;

            const std::string call = "world:" + std::string(name);
            if (!options->is<sol::table>())
            {
                MIR_LOG_ERROR("script: {}: options must be a table", call);
                return false;
            }

            for (const auto &[key, value] : options->as<sol::table>())
            {
                const std::string option = key.is<std::string>() ? key.as<std::string>() : std::string();
                if (option == "duration")
                {
                    if (!ReadSeconds(value, params.mDuration, call, "options.duration"))
                        return false;
                }
                else if (option == "delay")
                {
                    if (!ReadSeconds(value, params.mDelay, call, "options.delay"))
                        return false;
                }
                else if (option == "ease")
                {
                    const std::optional<Ease> ease =
                        value.get_type() == sol::type::string ? EaseFromName(value.as<std::string>()) : std::nullopt;
                    if (!ease)
                    {
                        MIR_LOG_ERROR("script: {}: options.ease must be Linear, In, Out, InOut, StrongIn, StrongOut, "
                                      "Back or Bounce", call);
                        return false;
                    }
                    params.mEase = *ease;
                }
                else if (option == "blocking")
                {
                    if (value.get_type() != sol::type::boolean)
                    {
                        MIR_LOG_ERROR("script: {}: options.blocking needs true or false", call);
                        return false;
                    }
                    params.mBlocking = value.as<bool>();
                }
                else if (option == "lane")
                {
                    const std::optional<uint64_t> lane = ReadLane(value, name, "options.lane");
                    if (!lane)
                        return false;
                    params.mLane = *lane;
                }
                else if (option == "clock")
                {
                    const std::string clock = value.get_type() == sol::type::string ? value.as<std::string>() : std::string();
                    if (clock != "game" && clock != "ui")
                    {
                        MIR_LOG_ERROR("script: {}: options.clock must be \"game\" or \"ui\"", call);
                        return false;
                    }
                    params.mClock = clock == "ui" ? ActionClock::UI : ActionClock::Game;
                }
                else
                {
                    MIR_LOG_ERROR("script: {} has no option '{}', the options are duration, delay, ease, blocking, "
                                  "lane and clock", call, option);
                    return false;
                }
            }
            return true;
        }

        LuaHandle WorldMoveTo(World &world, Entity entity, const sol::object &position, sol::optional<sol::object> options)
        {
            ActionParams params;
            const std::optional<WorldVec3> goal = ReadWorldVec3(position, "world:move_to", "position");
            ActionScheduler *actions = SchedulerOf(world, "move_to");
            if (!goal || actions == nullptr || !ReadOptions(options, params, "move_to"))
                return sol::nullopt;
            return ToLua(actions->MoveTo(entity, *goal, params));
        }

        LuaHandle WorldRotateTo(World &world, Entity entity, const sol::object &rotation, sol::optional<sol::object> options)
        {
            ActionParams params;
            glm::quat goal{ 1.0f, 0.0f, 0.0f, 0.0f };
            ActionScheduler *actions = SchedulerOf(world, "rotate_to");
            if (!ReadQuat(rotation, goal, "world:rotate_to", "rotation") || actions == nullptr ||
                !ReadOptions(options, params, "rotate_to"))
                return sol::nullopt;
            return ToLua(actions->RotateTo(entity, goal, params));
        }

        LuaHandle WorldScaleTo(World &world, Entity entity, const sol::object &scale, sol::optional<sol::object> options)
        {
            ActionParams params;
            glm::vec3 goal{ 1.0f };
            ActionScheduler *actions = SchedulerOf(world, "scale_to");
            if (!ReadVec3(scale, goal, "world:scale_to", "scale") || actions == nullptr ||
                !ReadOptions(options, params, "scale_to"))
                return sol::nullopt;
            return ToLua(actions->ScaleTo(entity, goal, params));
        }

        LuaHandle WorldTweenValue(World &world, std::string_view name, const sol::object &goal, sol::optional<sol::object> options)
        {
            ActionParams params;
            double number = 0.0;
            ActionScheduler *actions = SchedulerOf(world, "tween_value");
            if (!ReadNumber(goal, number, "world:tween_value", "goal") || actions == nullptr ||
                !ReadOptions(options, params, "tween_value"))
                return sol::nullopt;
            return ToLua(actions->TweenValue(name, static_cast<float>(number), params));
        }

        bool WorldSetValue(World &world, std::string_view name, const sol::object &value)
        {
            double number = 0.0;
            ActionScheduler *actions = SchedulerOf(world, "set_value");
            if (!ReadNumber(value, number, "world:set_value", "value") || actions == nullptr || name.empty())
                return false;
            actions->SetValue(name, static_cast<float>(number));
            return true;
        }

        double WorldValue(World &world, std::string_view name, sol::optional<double> fallback)
        {
            const double otherwise = fallback.value_or(0.0);
            const ActionScheduler *actions = world.TryResource<ActionScheduler>();
            return actions != nullptr ? actions->Value(name, static_cast<float>(otherwise)) : otherwise;
        }

        bool WorldActionRunning(World &world, const sol::object &handle)
        {
            const std::optional<ActionHandle> read = ReadHandle(handle, "action_running");
            const ActionScheduler *actions = world.TryResource<ActionScheduler>();
            return read && actions != nullptr && actions->IsRunning(*read);
        }

        void WorldCancelAction(World &world, const sol::object &handle)
        {
            const std::optional<ActionHandle> read = ReadHandle(handle, "cancel_action");
            if (ActionScheduler *actions = SchedulerOf(world, "cancel_action"); read && actions != nullptr)
                actions->Cancel(*read);
        }

        void WorldCancelActions(World &world, Entity entity)
        {
            if (ActionScheduler *actions = SchedulerOf(world, "cancel_actions"))
                actions->CancelAll(entity);
        }

        void WorldCancelValue(World &world, std::string_view name)
        {
            if (ActionScheduler *actions = SchedulerOf(world, "cancel_value"))
                actions->CancelValue(name);
        }

        void WorldCancelLane(World &world, const sol::object &lane)
        {
            const std::optional<uint64_t> read = ReadLane(lane, "cancel_lane", "lane");
            if (ActionScheduler *actions = SchedulerOf(world, "cancel_lane"); read && actions != nullptr)
                actions->CancelLane(*read);
        }

        bool WorldLaneBusy(World &world, const sol::object &lane)
        {
            const std::optional<uint64_t> read = ReadLane(lane, "lane_busy", "lane");
            const ActionScheduler *actions = world.TryResource<ActionScheduler>();
            return read && actions != nullptr && actions->IsLaneBusy(*read);
        }

        bool WorldSetLanePaused(World &world, const sol::object &lane, const sol::object &paused)
        {
            const std::optional<uint64_t> read = ReadLane(lane, "set_lane_paused", "lane");
            if (paused.get_type() != sol::type::boolean)
            {
                MIR_LOG_ERROR("script: world:set_lane_paused: paused needs true or false");
                return false;
            }
            ActionScheduler *actions = SchedulerOf(world, "set_lane_paused");
            if (!read || actions == nullptr)
                return false;
            actions->SetLanePaused(*read, paused.as<bool>());
            return true;
        }

        bool WorldSetLaneTimeScale(World &world, const sol::object &lane, const sol::object &scale)
        {
            const std::optional<uint64_t> read = ReadLane(lane, "set_lane_time_scale", "lane");
            float number = 1.0f;
            ActionScheduler *actions = SchedulerOf(world, "set_lane_time_scale");
            if (!read || !ReadSeconds(scale, number, "world:set_lane_time_scale", "scale") || actions == nullptr)
                return false;
            actions->SetLaneTimeScale(*read, number);
            return true;
        }

        bool WorldSetActionTimeScale(World &world, const sol::object &scale, sol::optional<std::string> clock)
        {
            float number = 1.0f;
            if (!ReadSeconds(scale, number, "world:set_action_time_scale", "scale"))
                return false;
            const std::string name = clock.value_or("game");
            if (name != "game" && name != "ui")
            {
                MIR_LOG_ERROR("script: world:set_action_time_scale: clock must be \"game\" or \"ui\"");
                return false;
            }
            ActionScheduler *actions = SchedulerOf(world, "set_action_time_scale");
            if (actions == nullptr)
                return false;
            actions->SetTimeScale(name == "ui" ? ActionClock::UI : ActionClock::Game, number);
            return true;
        }
    }

    void RegisterActionBindings(sol::state &lua)
    {
        sol::table worldType = lua["World"];
        worldType.set_function("move_to", &WorldMoveTo);
        worldType.set_function("rotate_to", &WorldRotateTo);
        worldType.set_function("scale_to", &WorldScaleTo);
        worldType.set_function("tween_value", &WorldTweenValue);
        worldType.set_function("set_value", &WorldSetValue);
        worldType.set_function("value", &WorldValue);
        worldType.set_function("action_running", &WorldActionRunning);
        worldType.set_function("cancel_action", &WorldCancelAction);
        worldType.set_function("cancel_actions", &WorldCancelActions);
        worldType.set_function("cancel_value", &WorldCancelValue);
        worldType.set_function("cancel_lane", &WorldCancelLane);
        worldType.set_function("lane_busy", &WorldLaneBusy);
        worldType.set_function("set_lane_paused", &WorldSetLanePaused);
        worldType.set_function("set_lane_time_scale", &WorldSetLaneTimeScale);
        worldType.set_function("set_action_time_scale", &WorldSetActionTimeScale);
    }
}
