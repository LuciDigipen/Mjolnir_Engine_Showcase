/**
 * @file UIBindings.cpp
 * @author Quy Dong Ta
 * @brief The ui object a script's OnUI is given, and the call that runs every script's OnUI in a game UI frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui_script/UIBindings.h>

#include <assets/AssetResolver.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>
#include <gameui/UILayout.h>
#include <gameui/UIWidgets.h>
#include <script/BindingApi.h>
#include <script/components/ScriptRef.h>

#include <sol/sol.hpp>

#include <cmath>
#include <format>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        [[noreturn]] void Fail(std::string_view call, std::string_view problem)
        {
            throw sol::error(std::format("ui:{}: {}", call, problem));
        }

        UIContext &Use(UIScriptFrame &frame, std::string_view call)
        {
            if (frame.mUI == nullptr)
                Fail(call, "ui is only valid inside the OnUI it was given to");
            return *frame.mUI;
        }

        float ReadFloat(const sol::object &value, std::string_view call, std::string_view argument)
        {
            if (value.get_type() != sol::type::number)
                Fail(call, std::format("{} must be a number", argument));
            const double number = value.as<double>();
            if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max())
                Fail(call, std::format("{} must be a finite number a float can hold", argument));
            return static_cast<float>(number);
        }

        std::string_view ReadText(const sol::object &value, std::string_view call, std::string_view argument)
        {
            if (value.get_type() != sol::type::string)
                Fail(call, std::format("{} must be a string", argument));
            return value.as<std::string_view>();
        }

        bool ReadBool(const sol::object &value, std::string_view call, std::string_view argument)
        {
            if (value.get_type() != sol::type::boolean)
                Fail(call, std::format("{} must be true or false", argument));
            return value.as<bool>();
        }

        UIRect ReadRect(const sol::object &value, std::string_view call, std::string_view argument = "rect")
        {
            if (value.get_type() != sol::type::table)
                Fail(call, std::format("{} must be a table {{ x, y, w, h }}", argument));
            const sol::table table = value.as<sol::table>();
            const auto field = [&](const char *key)
            { return ReadFloat(table[key].get<sol::object>(), call, std::format("{}.{}", argument, key)); };
            return UIRect{ field("x"), field("y"), field("w"), field("h") };
        }

        sol::table RectToLua(sol::state_view lua, const UIRect &rect)
        {
            return lua.create_table_with("x", rect.x, "y", rect.y, "w", rect.width, "h", rect.height);
        }

        // the owner is mixed in, so two entities running one script do not share a widget
        UIId MakeId(const UIScriptFrame &frame, std::string_view name, uint64_t index)
        {
            return UIId(name, PackEntity(frame.mOwner) * 0x9E3779B97F4A7C15ull + index);
        }

        uint64_t ReadIndex(const sol::object &value, std::string_view call, std::string_view argument)
        {
            const float number = ReadFloat(value, call, argument);
            if (number < 0.0f || number != std::floor(number))
                Fail(call, std::format("{} must be a whole number of 0 or more", argument));
            return static_cast<uint64_t>(number);
        }

        // a name, or { name, index } for a widget made in a loop
        UIId ReadLink(const UIScriptFrame &frame, const sol::object &value, std::string_view call,
            std::string_view argument)
        {
            if (value.get_type() == sol::type::string)
                return MakeId(frame, value.as<std::string_view>(), 0);
            if (value.get_type() != sol::type::table)
                Fail(call, std::format("{} must be a widget name or {{ name, index }}", argument));
            const sol::table table = value.as<sol::table>();
            return MakeId(frame, ReadText(table[1].get<sol::object>(), call, argument),
                ReadIndex(table[2].get<sol::object>(), call, argument));
        }

        struct Options
        {
            uint64_t mIndex = 0;
            UIWidgetOptions mWidget;
            bool mFocusable = false;
            UIColor mTint = kUIWhite;
            float mStep = 0.0f;
        };

        constexpr std::string_view kCommonOptions[] = { "index", "disabled", "hidden", "pointer_only", "links" };

        UIColor ReadTint(const sol::object &value, std::string_view call)
        {
            if (value.get_type() != sol::type::table)
                Fail(call, "options.tint must be a table { r, g, b, a }");
            const sol::table tint = value.as<sol::table>();
            const auto channel = [&](const char *name)
            {
                const sol::object part = tint[name].get<sol::object>();
                return part == sol::lua_nil ? 1.0f : ReadFloat(part, call, "options.tint");
            };
            return UIColor{ channel("r"), channel("g"), channel("b"), channel("a") };
        }

        UINavLinks ReadLinks(const UIScriptFrame &frame, const sol::object &value, std::string_view call)
        {
            if (value.get_type() != sol::type::table)
                Fail(call, "options.links must be a table { up, down, left, right }");
            const sol::table table = value.as<sol::table>();
            // UINavDirection's order
            constexpr const char *kDirections[] = { "up", "down", "left", "right" };
            UINavLinks links{};
            for (std::size_t i = 0; i < std::size(kDirections); ++i)
            {
                const sol::object link = table[kDirections[i]].get<sol::object>();
                if (link != sol::lua_nil)
                    links[i] = ReadLink(frame, link, call, "options.links");
            }
            return links;
        }

        Options ReadOptions(const UIScriptFrame &frame, const sol::object &value, std::string_view call,
            std::initializer_list<std::string_view> extra)
        {
            Options options;
            if (value == sol::lua_nil)
                return options;
            if (value.get_type() != sol::type::table)
                Fail(call, "options must be a table");

            const auto known = [&](std::string_view key)
            {
                for (const std::string_view name : kCommonOptions)
                    if (key == name)
                        return true;
                for (const std::string_view name : extra)
                    if (key == name)
                        return true;
                return false;
            };

            for (const auto &[keyObject, entry] : value.as<sol::table>())
            {
                const std::string_view key =
                    keyObject.get_type() == sol::type::string ? keyObject.as<std::string_view>() : std::string_view{};
                if (!known(key))
                {
                    std::string valid;
                    for (const std::string_view name : kCommonOptions)
                        valid += std::format("{}{}", valid.empty() ? "" : ", ", name);
                    for (const std::string_view name : extra)
                        valid += std::format(", {}", name);
                    Fail(call, std::format("unknown option; valid ones are {}", valid));
                }

                if (key == "index")
                    options.mIndex = ReadIndex(entry, call, "options.index");
                else if (key == "disabled")
                    options.mWidget.mDisabled = ReadBool(entry, call, "options.disabled");
                else if (key == "hidden")
                    options.mWidget.mHidden = ReadBool(entry, call, "options.hidden");
                else if (key == "pointer_only")
                    options.mWidget.mPointerOnly = ReadBool(entry, call, "options.pointer_only");
                else if (key == "links")
                    options.mWidget.mLinks = ReadLinks(frame, entry, call);
                else if (key == "focusable")
                    options.mFocusable = ReadBool(entry, call, "options.focusable");
                else if (key == "tint")
                    options.mTint = ReadTint(entry, call);
                else if (key == "step")
                    options.mStep = ReadFloat(entry, call, "options.step");
            }
            return options;
        }

        sol::table ResultToLua(sol::state_view lua, const UIWidgetResult &result)
        {
            return lua.create_table_with("clicked", result.mClicked, "hovered", result.mHovered, "held",
                result.mHeld, "focused", result.mFocused, "changed", result.mChanged);
        }

        UIDisplayOptions Display(const Options &options)
        {
            return UIDisplayOptions{ .mWidget = options.mWidget, .mFocusable = options.mFocusable };
        }

        UIAnchor ReadAnchor(const sol::object &value, std::string_view call)
        {
            static constexpr std::pair<std::string_view, UIAnchor> kAnchors[] = {
                { "top_left", kUITopLeft }, { "top", kUITop }, { "top_right", kUITopRight },
                { "left", kUILeft }, { "center", kUICenter }, { "right", kUIRight },
                { "bottom_left", kUIBottomLeft }, { "bottom", kUIBottom }, { "bottom_right", kUIBottomRight },
            };
            const std::string_view name = ReadText(value, call, "anchor");
            for (const auto &[key, anchor] : kAnchors)
                if (key == name)
                    return anchor;
            Fail(call, std::format("unknown anchor '{}'; valid ones are top_left, top, top_right, left, center, "
                "right, bottom_left, bottom, bottom_right", name));
        }

        sol::protected_function ReadFunction(const sol::object &value, std::string_view call)
        {
            if (value.get_type() != sol::type::function)
                Fail(call, "the last argument must be a function");
            return value.as<sol::protected_function>();
        }

        // pops what it pushed whether fn returns, errors or throws
        template <typename Pop>
        void CallScoped(const sol::protected_function &fn, std::string_view call, Pop pop)
        {
            std::optional<sol::protected_function_result> result;
            try
            {
                result.emplace(fn());
            }
            catch (...)
            {
                pop();
                throw;
            }
            pop();
            if (!result->valid())
            {
                const sol::error error = *result;
                Fail(call, error.what());
            }
        }

        UISprite LoadSprite(UIScriptFrame &frame, UIContext &ui, std::string_view path, float slice)
        {
            const std::string key = std::format("{}#{}", path, slice);
            if (const auto it = frame.mSprites.find(key); it != frame.mSprites.end())
                return it->second;

            UISprite sprite;
            if (frame.mAssets == nullptr || frame.mResolver == nullptr)
                MIR_LOG_ERROR("ui:sprite: no assets to load '{}' from", path);
            else
            {
                std::string error;
                const AssetId id = frame.mResolver->Resolve(path, &error);
                if (id.IsNull())
                    MIR_LOG_ERROR("ui:sprite: '{}': {}", path, error);
                else
                {
                    sprite = ui.Textures().Load(*frame.mAssets, id);
                    if (!sprite.IsValid())
                        MIR_LOG_ERROR("ui:sprite: '{}' is not a texture that loads", path);
                    sprite.mSlice = UIEdges{ slice, slice, slice, slice };
                }
            }
            frame.mSprites.emplace(key, sprite);
            return sprite;
        }

        float ReadOptionalFloat(const sol::object &value, std::string_view call, std::string_view argument)
        {
            return value == sol::lua_nil ? 0.0f : ReadFloat(value, call, argument);
        }

        struct LuaStack
        {
            UIStack mStack;
        };
    }

    void RegisterUIBindings(sol::state &lua)
    {
        lua.new_usertype<UISprite>("UISprite", sol::no_constructor);

        lua.new_usertype<LuaStack>("UIStack", sol::no_constructor,
            "next", [](LuaStack &self, const sol::object &size, sol::this_state state)
            { return RectToLua(state, self.mStack.Next(ReadFloat(size, "next", "size"))); },
            "last", [](LuaStack &self, const sol::object &size, sol::this_state state)
            { return RectToLua(state, self.mStack.Last(ReadFloat(size, "last", "size"))); },
            "rest", [](LuaStack &self, sol::this_state state) { return RectToLua(state, self.mStack.Rest()); },
            "remaining", [](const LuaStack &self) { return self.mStack.Remaining(); });

        sol::usertype<UIScriptFrame> ui = lua.new_usertype<UIScriptFrame>("UI", sol::no_constructor);

        // the area and layout, in UI units
        ui["bounds"] = [](UIScriptFrame &self, sol::this_state state)
        { return RectToLua(state, Use(self, "bounds").Bounds()); };
        ui["safe_area"] = [](UIScriptFrame &self, sol::this_state state)
        { return RectToLua(state, Use(self, "safe_area").SafeArea()); };
        ui["scale"] = [](UIScriptFrame &self) { return Use(self, "scale").Scale(); };
        ui["anchor"] = [](UIScriptFrame &, const sol::object &rect, const sol::object &where, const sol::object &width,
                           const sol::object &height, sol::this_state state)
        {
            return RectToLua(state, Anchor(ReadRect(rect, "anchor"), ReadAnchor(where, "anchor"),
                ReadFloat(width, "anchor", "width"), ReadFloat(height, "anchor", "height")));
        };
        ui["inset"] = [](UIScriptFrame &, const sol::object &rect, const sol::object &left, const sol::object &top,
                          const sol::object &right, const sol::object &bottom, sol::this_state state)
        {
            const UIRect area = ReadRect(rect, "inset");
            const float all = ReadFloat(left, "inset", "left");
            if (top == sol::lua_nil && right == sol::lua_nil && bottom == sol::lua_nil)
                return RectToLua(state, Inset(area, all));
            return RectToLua(state, Inset(area, UIEdges{ all, ReadFloat(top, "inset", "top"),
                ReadFloat(right, "inset", "right"), ReadFloat(bottom, "inset", "bottom") }));
        };
        ui["row"] = [](UIScriptFrame &, const sol::object &rect, const sol::object &gap)
        { return LuaStack{ UIStack::Row(ReadRect(rect, "row"), ReadOptionalFloat(gap, "row", "gap")) }; };
        ui["column"] = [](UIScriptFrame &, const sol::object &rect, const sol::object &gap)
        { return LuaStack{ UIStack::Column(ReadRect(rect, "column"), ReadOptionalFloat(gap, "column", "gap")) }; };

        // widgets
        ui["panel"] = [](UIScriptFrame &self, const sol::object &rect)
        { Panel(Use(self, "panel"), ReadRect(rect, "panel")); };
        ui["label"] = [](UIScriptFrame &self, const sol::object &name, const sol::object &rect,
                          const sol::object &text, const sol::object &opts, sol::this_state state)
        {
            UIContext &context = Use(self, "label");
            const Options options = ReadOptions(self, opts, "label", { "focusable" });
            return ResultToLua(state, Label(context, MakeId(self, ReadText(name, "label", "name"), options.mIndex),
                ReadRect(rect, "label"), ReadText(text, "label", "text"), Display(options)));
        };
        ui["button"] = [](UIScriptFrame &self, const sol::object &name, const sol::object &rect,
                           const sol::object &text, const sol::object &opts, sol::this_state state)
        {
            UIContext &context = Use(self, "button");
            const Options options = ReadOptions(self, opts, "button", {});
            return ResultToLua(state, Button(context, MakeId(self, ReadText(name, "button", "name"), options.mIndex),
                ReadRect(rect, "button"), ReadText(text, "button", "text"), options.mWidget));
        };
        ui["image"] = [](UIScriptFrame &self, const sol::object &name, const sol::object &rect,
                          const sol::object &sprite, const sol::object &opts, sol::this_state state)
        {
            UIContext &context = Use(self, "image");
            if (!sprite.is<UISprite>())
                Fail("image", "sprite must be what ui:sprite returned");
            const Options options = ReadOptions(self, opts, "image", { "focusable", "tint" });
            return ResultToLua(state, Image(context, MakeId(self, ReadText(name, "image", "name"), options.mIndex),
                ReadRect(rect, "image"), sprite.as<UISprite>(), options.mTint, Display(options)));
        };
        ui["bar"] = [](UIScriptFrame &self, const sol::object &name, const sol::object &rect,
                        const sol::object &fraction, const sol::object &opts, sol::this_state state)
        {
            UIContext &context = Use(self, "bar");
            const Options options = ReadOptions(self, opts, "bar", { "focusable" });
            return ResultToLua(state, Bar(context, MakeId(self, ReadText(name, "bar", "name"), options.mIndex),
                ReadRect(rect, "bar"), ReadFloat(fraction, "bar", "fraction"), Display(options)));
        };
        ui["toggle"] = [](UIScriptFrame &self, const sol::object &name, const sol::object &rect,
                           const sol::object &text, const sol::object &value, const sol::object &opts,
                           sol::this_state state)
        {
            UIContext &context = Use(self, "toggle");
            const Options options = ReadOptions(self, opts, "toggle", {});
            bool on = ReadBool(value, "toggle", "value");
            const UIWidgetResult result = Toggle(context, MakeId(self, ReadText(name, "toggle", "name"),
                options.mIndex), ReadRect(rect, "toggle"), ReadText(text, "toggle", "text"), on, options.mWidget);
            return std::make_tuple(on, ResultToLua(state, result));
        };
        ui["slider"] = [](UIScriptFrame &self, const sol::object &name, const sol::object &rect,
                           const sol::object &value, const sol::object &min, const sol::object &max,
                           const sol::object &opts, sol::this_state state)
        {
            UIContext &context = Use(self, "slider");
            const Options options = ReadOptions(self, opts, "slider", { "step" });
            const float low = ReadFloat(min, "slider", "min");
            const float high = ReadFloat(max, "slider", "max");
            if (!(low < high))
                Fail("slider", "min must be less than max");
            float current = ReadFloat(value, "slider", "value");
            const UIWidgetResult result = Slider(context, MakeId(self, ReadText(name, "slider", "name"),
                options.mIndex), ReadRect(rect, "slider"), current, low, high, options.mStep, options.mWidget);
            return std::make_tuple(current, ResultToLua(state, result));
        };
        ui["sprite"] = [](UIScriptFrame &self, const sol::object &path, const sol::object &slice,
                           sol::this_state state) -> sol::object
        {
            UIContext &context = Use(self, "sprite");
            const UISprite sprite = LoadSprite(self, context, ReadText(path, "sprite", "path"),
                ReadOptionalFloat(slice, "sprite", "slice"));
            if (!sprite.IsValid())
                return sol::lua_nil;
            return sol::make_object(state, sprite);
        };

        // scoped, so nothing a script pushes outlives the function it pushed it for
        ui["with_opacity"] = [](UIScriptFrame &self, const sol::object &alpha, const sol::object &fn)
        {
            UIDrawList &draw = Use(self, "with_opacity").DrawList();
            const float value = ReadFloat(alpha, "with_opacity", "alpha");
            const sol::protected_function body = ReadFunction(fn, "with_opacity");
            draw.PushOpacity(value);
            CallScoped(body, "with_opacity", [&] { draw.PopOpacity(); });
        };
        ui["with_clip"] = [](UIScriptFrame &self, const sol::object &rect, const sol::object &fn)
        {
            UIDrawList &draw = Use(self, "with_clip").DrawList();
            const UIRect clip = ReadRect(rect, "with_clip");
            const sol::protected_function body = ReadFunction(fn, "with_clip");
            draw.PushClip(clip);
            CallScoped(body, "with_clip", [&] { draw.PopClip(); });
        };

        // the frame
        ui["hold_input"] = [](UIScriptFrame &self) { Use(self, "hold_input").HoldInput(); };
        ui["hide_cursor"] = [](UIScriptFrame &self) { Use(self, "hide_cursor").HideCursor(); };
        ui["cancel_pressed"] = [](UIScriptFrame &self) { return Use(self, "cancel_pressed").CancelPressed(); };
        ui["pause_pressed"] = [](UIScriptFrame &self) { return Use(self, "pause_pressed").PausePressed(); };
        ui["focus_visible"] = [](UIScriptFrame &self) { return Use(self, "focus_visible").FocusVisible(); };
        ui["set_focus"] = [](UIScriptFrame &self, const sol::object &name, const sol::object &index)
        {
            UIContext &context = Use(self, "set_focus");
            context.State().SetFocus(MakeId(self, ReadText(name, "set_focus", "name"),
                index == sol::lua_nil ? 0 : ReadIndex(index, "set_focus", "index")));
        };
        ui["measure_text"] = [](UIScriptFrame &self, const sol::object &text)
        {
            const UISize size = Use(self, "measure_text").MeasureText(ReadText(text, "measure_text", "text"));
            return std::make_tuple(size.width, size.height);
        };
        ui["prompt"] = [](UIScriptFrame &self, const sol::object &action, sol::this_state state)
        {
            const UIPrompt prompt = Use(self, "prompt").Prompt(ReadText(action, "prompt", "action"));
            const sol::object label = prompt.mLabel.empty() ? sol::object(sol::lua_nil)
                                                            : sol::make_object(state, std::string(prompt.mLabel));
            return std::make_tuple(label, prompt.mGamepad);
        };
    }

    void DrawScriptScreens(ScriptHost &host, World &world, const std::shared_ptr<UIScriptFrame> &frame,
        UIContext &ui, float dt)
    {
        // in Play, paused too, so a game's own pause menu can resume, scripts do not run in Edit
        if (!world.HasResource<EngineModeState>() || !IsPlaying(world))
            return;

        // gathered first: an OnUI may spawn or destroy entities
        struct Instance
        {
            Entity mEntity;
            int32_t mRef = -1;
        };
        std::vector<Instance> instances;
        world.GetOrCreateQuery<ScriptRef>().ForEach([&](Entity entity, const ScriptRef &ref)
            {
                if (ref.instanceRef >= 0 && ref.started)
                    instances.push_back({ entity, ref.instanceRef });
            });
        if (instances.empty())
            return;

        sol::state_view lua = LuaState(host);
        const std::vector<sol::object> args = { sol::make_object(lua, frame), sol::make_object(lua, dt) };
        UIDrawList &draw = ui.DrawList();
        frame->mUI = &ui;
        for (const Instance &instance : instances)
        {
            const ScriptRef *ref =
                world.IsAlive(instance.mEntity) ? world.GetComponent<ScriptRef>(instance.mEntity) : nullptr;
            if (ref == nullptr || ref->instanceRef != instance.mRef || !ref->started)
                continue;

            frame->mOwner = instance.mEntity;
            const std::size_t clips = draw.ClipDepth();
            const std::size_t fades = draw.OpacityDepth();
            CallScriptCallback(host, world, instance.mEntity, instance.mRef, "OnUI", args);
            while (draw.ClipDepth() > clips)
                draw.PopClip();
            while (draw.OpacityDepth() > fades)
                draw.PopOpacity();
        }
        frame->mUI = nullptr;
        frame->mOwner = kNullEntity;
    }
}
