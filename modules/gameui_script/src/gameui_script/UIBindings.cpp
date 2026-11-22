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
#include <numbers>
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

        // what a widget's text options change in the style, for that widget only
        struct TextOptions
        {
            std::optional<UIFont> mFont;
            std::optional<float> mSize;
            std::optional<UIColor> mColor;
            std::optional<UITextAlign> mAlign;
            std::optional<UIColor> mShadowColor;
            float mShadowX = 1.0f;
            float mShadowY = 1.0f;
            std::optional<UIColor> mOutlineColor;
            float mOutlineWidth = 1.0f;
        };

        struct Options
        {
            uint64_t mIndex = 0;
            UIWidgetOptions mWidget;
            bool mFocusable = false;
            UIColor mTint = kUIWhite;
            float mStep = 0.0f;
            TextOptions mText;
        };

        constexpr std::string_view kCommonOptions[] = { "index", "disabled", "hidden", "pointer_only", "links" };
        constexpr std::string_view kTextOptions[] = { "font", "size", "color", "align", "shadow", "outline" };

        // { r, g, b, a } by name or by position; a missing channel is 1
        UIColor ReadColor(const sol::object &value, std::string_view call, std::string_view argument)
        {
            if (value.get_type() != sol::type::table)
                Fail(call, std::format("{} must be a table {{ r, g, b, a }}", argument));
            const sol::table table = value.as<sol::table>();
            const bool positional = table[1].get<sol::object>() != sol::lua_nil;
            const auto channel = [&](const char *name, int position)
            {
                const sol::object part = positional ? table[position].get<sol::object>() : table[name].get<sol::object>();
                return part == sol::lua_nil ? 1.0f : ReadFloat(part, call, argument);
            };
            return UIColor{ channel("r", 1), channel("g", 2), channel("b", 3), channel("a", 4) };
        }

        UIColor ReadTint(const sol::object &value, std::string_view call)
        {
            return ReadColor(value, call, "options.tint");
        }

        UIFont ReadFont(const sol::object &value, std::string_view call, std::string_view argument)
        {
            if (!value.is<UIFont>())
                Fail(call, std::format("{} must be what ui:font returned", argument));
            return value.as<UIFont>();
        }

        float ReadSize(const sol::object &value, std::string_view call, std::string_view argument)
        {
            const float size = ReadFloat(value, call, argument);
            if (size <= 0.0f)
                Fail(call, std::format("{} must be more than 0", argument));
            return size;
        }

        UITextAlign ReadAlign(const sol::object &value, std::string_view call)
        {
            const std::string_view name = ReadText(value, call, "options.align");
            if (name == "left")
                return UITextAlign::Left;
            if (name == "center")
                return UITextAlign::Center;
            if (name == "right")
                return UITextAlign::Right;
            Fail(call, std::format("unknown align '{}'; valid ones are left, center, right", name));
        }

        // false turns off what the style has, true is black at the defaults, a table sets what it names
        template <typename Fields>
        std::optional<UIColor> ReadEffect(const sol::object &value, std::string_view call, std::string_view argument,
            Fields fields)
        {
            constexpr UIColor kBlack{ 0.0f, 0.0f, 0.0f, 1.0f };
            if (value.get_type() == sol::type::boolean)
                return value.as<bool>() ? kBlack : UIColor{ 0.0f, 0.0f, 0.0f, 0.0f };
            if (value.get_type() != sol::type::table)
                Fail(call, std::format("{} must be true, false or a table", argument));
            const sol::table table = value.as<sol::table>();
            fields(table);
            const sol::object color = table["color"].get<sol::object>();
            return color == sol::lua_nil ? kBlack : ReadColor(color, call, std::format("{}.color", argument));
        }

        void ReadTextOption(TextOptions &text, std::string_view key, const sol::object &entry, std::string_view call)
        {
            if (key == "font")
                text.mFont = ReadFont(entry, call, "options.font");
            else if (key == "size")
                text.mSize = ReadSize(entry, call, "options.size");
            else if (key == "color")
                text.mColor = ReadColor(entry, call, "options.color");
            else if (key == "align")
                text.mAlign = ReadAlign(entry, call);
            else if (key == "shadow")
                text.mShadowColor = ReadEffect(entry, call, "options.shadow", [&](const sol::table &table)
                    {
                        const sol::object x = table["x"].get<sol::object>();
                        const sol::object y = table["y"].get<sol::object>();
                        if (x != sol::lua_nil)
                            text.mShadowX = ReadFloat(x, call, "options.shadow.x");
                        if (y != sol::lua_nil)
                            text.mShadowY = ReadFloat(y, call, "options.shadow.y");
                    });
            else if (key == "outline")
                text.mOutlineColor = ReadEffect(entry, call, "options.outline", [&](const sol::table &table)
                    {
                        const sol::object width = table["width"].get<sol::object>();
                        if (width != sol::lua_nil)
                            text.mOutlineWidth = ReadSize(width, call, "options.outline.width");
                    });
        }

        // a widget's text options over the style while it draws, put back after, whatever happens
        class TextStyleScope
        {
        public:
            TextStyleScope(UIContext &ui, const TextOptions &text) : mStyle(ui.Style()), mSaved(ui.Style())
            {
                if (text.mFont)
                    mStyle.mFont = *text.mFont;
                if (text.mSize)
                    mStyle.mFontSize = *text.mSize;
                if (text.mColor)
                    mStyle.mTextColor = *text.mColor;
                if (text.mAlign)
                    mStyle.mTextAlign = *text.mAlign;
                if (text.mShadowColor)
                {
                    mStyle.mTextShadowColor = *text.mShadowColor;
                    mStyle.mTextShadowX = text.mShadowX;
                    mStyle.mTextShadowY = text.mShadowY;
                }
                if (text.mOutlineColor)
                {
                    mStyle.mTextOutlineColor = *text.mOutlineColor;
                    mStyle.mTextOutlineWidth = text.mOutlineWidth;
                }
            }
            ~TextStyleScope() { mStyle = mSaved; }

            TextStyleScope(const TextStyleScope &) = delete;
            TextStyleScope &operator=(const TextStyleScope &) = delete;

        private:
            UIStyle &mStyle;
            UIStyle mSaved;
        };

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
            std::initializer_list<std::string_view> extra, bool text = false)
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
                if (text)
                    for (const std::string_view name : kTextOptions)
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
                    if (text)
                        for (const std::string_view name : kTextOptions)
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
                else
                    ReadTextOption(options.mText, key, entry, call);
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

        UIFont LoadFont(UIScriptFrame &frame, UIContext &ui, std::string_view path)
        {
            const std::string key(path);
            if (const auto it = frame.mFonts.find(key); it != frame.mFonts.end())
                return it->second;

            UIFont font;
            if (frame.mAssets == nullptr || frame.mResolver == nullptr)
                MIR_LOG_ERROR("ui:font: no assets to load '{}' from", path);
            else
            {
                std::string error;
                const AssetId id = frame.mResolver->Resolve(path, &error);
                if (id.IsNull())
                    MIR_LOG_ERROR("ui:font: '{}': {}", path, error);
                else
                {
                    font = ui.Fonts().Load(*frame.mAssets, id);
                    if (!font.IsValid())
                        MIR_LOG_ERROR("ui:font: '{}' is not a font that loads", path);
                }
            }
            frame.mFonts.emplace(key, font);
            return font;
        }

        float ReadOptionalFloat(const sol::object &value, std::string_view call, std::string_view argument)
        {
            return value == sol::lua_nil ? 0.0f : ReadFloat(value, call, argument);
        }

        constexpr float kRadiansPerDegree = std::numbers::pi_v<float> / 180.0f;

        // the most points one shape call takes
        constexpr std::size_t kMostPoints = 4096;

        float ReadAngle(const sol::object &value, std::string_view call, std::string_view argument)
        {
            return ReadFloat(value, call, argument) * kRadiansPerDegree;
        }

        // { x, y } by position or by name
        UIPoint ReadXY(const sol::object &value, std::string_view call, std::string_view argument)
        {
            if (value.get_type() != sol::type::table)
                Fail(call, std::format("{} must be a table {{ x, y }}", argument));
            const sol::table table = value.as<sol::table>();
            const bool positional = table[1].get<sol::object>() != sol::lua_nil;
            return UIPoint{ ReadFloat(positional ? table[1].get<sol::object>() : table["x"].get<sol::object>(), call,
                                std::format("{}.x", argument)),
                ReadFloat(positional ? table[2].get<sol::object>() : table["y"].get<sol::object>(), call,
                    std::format("{}.y", argument)) };
        }

        // { x1, y1, x2, y2, ... }
        std::vector<UIPoint> ReadPoints(const sol::object &value, std::string_view call, std::size_t fewest)
        {
            if (value.get_type() != sol::type::table)
                Fail(call, "points must be a table { x1, y1, x2, y2, ... }");
            const sol::table table = value.as<sol::table>();
            const std::size_t numbers = table.size();
            if (numbers % 2 != 0)
                Fail(call, "points must be x, y pairs: { x1, y1, x2, y2, ... }");
            if (numbers / 2 < fewest)
                Fail(call, std::format("needs at least {} points", fewest));
            if (numbers / 2 > kMostPoints)
                Fail(call, std::format("takes at most {} points", kMostPoints));

            std::vector<UIPoint> points;
            points.reserve(numbers / 2);
            for (std::size_t i = 1; i < numbers; i += 2)
                points.push_back(UIPoint{ ReadFloat(table[i].get<sol::object>(), call, "points"),
                    ReadFloat(table[i + 1].get<sol::object>(), call, "points") });
            return points;
        }

        // fails with the valid keys when the table has another
        void CheckKeys(const sol::table &table, std::string_view call, std::string_view what,
            std::initializer_list<std::string_view> keys)
        {
            for (const auto &[keyObject, entry] : table)
            {
                const std::string_view key =
                    keyObject.get_type() == sol::type::string ? keyObject.as<std::string_view>() : std::string_view{};
                bool known = false;
                for (const std::string_view name : keys)
                    known = known || key == name;
                if (known)
                    continue;

                std::string valid;
                for (const std::string_view name : keys)
                    valid += std::format("{}{}", valid.empty() ? "" : ", ", name);
                Fail(call, std::format("unknown {}; valid ones are {}", what, valid));
            }
        }

        // { length, gap, offset } by position or by name; the gap defaults to the length
        void ReadDash(const sol::object &value, std::string_view call, UIStroke &stroke)
        {
            if (value.get_type() != sol::type::table)
                Fail(call, "options.dash must be a table { length, gap, offset }");
            const sol::table table = value.as<sol::table>();
            const bool positional = table[1].get<sol::object>() != sol::lua_nil;
            if (!positional)
                CheckKeys(table, call, "dash key", { "length", "gap", "offset" });
            const auto part = [&](const char *name, int position)
            { return positional ? table[position].get<sol::object>() : table[name].get<sol::object>(); };

            stroke.mDash = ReadSize(part("length", 1), call, "options.dash.length");
            const sol::object gap = part("gap", 2);
            stroke.mGap = gap == sol::lua_nil ? stroke.mDash : ReadSize(gap, call, "options.dash.gap");
            stroke.mDashOffset = ReadOptionalFloat(part("offset", 3), call, "options.dash.offset");
        }

        // { from = { x, y }, to = { x, y }, from_color, to_color }, each colour white unless given
        UIGradient ReadGradient(const sol::object &value, std::string_view call)
        {
            if (value.get_type() != sol::type::table)
                Fail(call, "options.gradient must be a table { from, to, from_color, to_color }");
            const sol::table table = value.as<sol::table>();
            CheckKeys(table, call, "gradient key", { "from", "to", "from_color", "to_color" });

            UIGradient gradient{ .mFrom = ReadXY(table["from"].get<sol::object>(), call, "options.gradient.from"),
                .mTo = ReadXY(table["to"].get<sol::object>(), call, "options.gradient.to") };
            if (gradient.mFrom == gradient.mTo)
                Fail(call, "options.gradient.from and options.gradient.to must be different points");
            const sol::object fromColor = table["from_color"].get<sol::object>();
            const sol::object toColor = table["to_color"].get<sol::object>();
            if (fromColor != sol::lua_nil)
                gradient.mFromColor = ReadColor(fromColor, call, "options.gradient.from_color");
            if (toColor != sol::lua_nil)
                gradient.mToColor = ReadColor(toColor, call, "options.gradient.to_color");
            return gradient;
        }

        struct ShapeOptions
        {
            UIColor mColor = kUIWhite;
            UIStroke mStroke;
            // a width or a dash outlines a shape that would otherwise be filled
            bool mStroked = false;
            bool mClosed = false;
            float mRounding = 0.0f;
            UIGradient mGradient;
        };

        ShapeOptions ReadShapeOptions(const sol::object &value, std::string_view call,
            std::initializer_list<std::string_view> keys)
        {
            ShapeOptions options;
            if (value == sol::lua_nil)
                return options;
            if (value.get_type() != sol::type::table)
                Fail(call, "options must be a table");
            const sol::table table = value.as<sol::table>();
            CheckKeys(table, call, "option", keys);

            for (const auto &[keyObject, entry] : table)
            {
                const std::string_view key = keyObject.as<std::string_view>();
                if (key == "color")
                    options.mColor = ReadColor(entry, call, "options.color");
                else if (key == "width")
                {
                    options.mStroke.mWidth = ReadSize(entry, call, "options.width");
                    options.mStroked = true;
                }
                else if (key == "dash")
                {
                    ReadDash(entry, call, options.mStroke);
                    options.mStroked = true;
                }
                else if (key == "gradient")
                    options.mGradient = ReadGradient(entry, call);
                else if (key == "closed")
                    options.mClosed = ReadBool(entry, call, "options.closed");
                else if (key == "rounding")
                {
                    options.mRounding = ReadFloat(entry, call, "options.rounding");
                    if (options.mRounding < 0.0f)
                        Fail(call, "options.rounding must be 0 or more");
                }
            }
            return options;
        }

        // { offset = { x, y }, rotate = degrees, scale = s or { x, y }, pivot = { x, y } }: scaled, then turned,
        // both about the pivot, then moved
        UITransform ReadTransform(const sol::object &value, std::string_view call)
        {
            if (value.get_type() != sol::type::table)
                Fail(call, "transform must be a table { offset, rotate, scale, pivot }");
            const sol::table table = value.as<sol::table>();
            CheckKeys(table, call, "transform key", { "offset", "rotate", "scale", "pivot" });

            const sol::object offsetValue = table["offset"].get<sol::object>();
            const sol::object rotateValue = table["rotate"].get<sol::object>();
            const sol::object scaleValue = table["scale"].get<sol::object>();
            const sol::object pivotValue = table["pivot"].get<sol::object>();

            const UIPoint pivot = pivotValue == sol::lua_nil ? UIPoint{} : ReadXY(pivotValue, call, "transform.pivot");
            UIPoint scale{ 1.0f, 1.0f };
            if (scaleValue.get_type() == sol::type::number)
            {
                const float uniform = ReadFloat(scaleValue, call, "transform.scale");
                scale = UIPoint{ uniform, uniform };
            }
            else if (scaleValue != sol::lua_nil)
                scale = ReadXY(scaleValue, call, "transform.scale");
            const float angle = rotateValue == sol::lua_nil ? 0.0f : ReadAngle(rotateValue, call, "transform.rotate");
            const UIPoint offset =
                offsetValue == sol::lua_nil ? UIPoint{} : ReadXY(offsetValue, call, "transform.offset");

            return UITransform::Translate(offset.x, offset.y) * UITransform::Rotate(angle, pivot) *
                   UITransform::Scale(scale.x, scale.y, pivot);
        }

        void DrawTransformed(UIScriptFrame &self, std::string_view call, const UITransform &transform,
            const sol::object &fn)
        {
            UIDrawList &draw = Use(self, call).DrawList();
            const sol::protected_function body = ReadFunction(fn, call);
            draw.PushTransform(transform);
            CallScoped(body, call, [&] { draw.PopTransform(); });
        }

        struct LuaStack
        {
            UIStack mStack;
        };
    }

    void RegisterUIBindings(sol::state &lua)
    {
        lua.new_usertype<UISprite>("UISprite", sol::no_constructor);
        lua.new_usertype<UIFont>("UIFont", sol::no_constructor);

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
            const Options options = ReadOptions(self, opts, "label", { "focusable" }, true);
            const TextStyleScope styled(context, options.mText);
            return ResultToLua(state, Label(context, MakeId(self, ReadText(name, "label", "name"), options.mIndex),
                ReadRect(rect, "label"), ReadText(text, "label", "text"), Display(options)));
        };
        ui["button"] = [](UIScriptFrame &self, const sol::object &name, const sol::object &rect,
                           const sol::object &text, const sol::object &opts, sol::this_state state)
        {
            UIContext &context = Use(self, "button");
            const Options options = ReadOptions(self, opts, "button", {}, true);
            const TextStyleScope styled(context, options.mText);
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
            const Options options = ReadOptions(self, opts, "toggle", {}, true);
            const TextStyleScope styled(context, options.mText);
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

        ui["font"] = [](UIScriptFrame &self, const sol::object &path, sol::this_state state) -> sol::object
        {
            UIContext &context = Use(self, "font");
            const UIFont font = LoadFont(self, context, ReadText(path, "font", "path"));
            if (!font.IsValid())
                return sol::lua_nil;
            return sol::make_object(state, font);
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
        ui["with_offset"] = [](UIScriptFrame &self, const sol::object &x, const sol::object &y, const sol::object &fn)
        {
            DrawTransformed(self, "with_offset",
                UITransform::Translate(ReadFloat(x, "with_offset", "x"), ReadFloat(y, "with_offset", "y")), fn);
        };
        // (degrees, fn) about the origin, or (degrees, px, py, fn) about a pivot
        ui["with_rotation"] = [](UIScriptFrame &self, const sol::object &degrees, const sol::object &x,
                                  const sol::object &y, const sol::object &fn)
        {
            const float angle = ReadAngle(degrees, "with_rotation", "degrees");
            if (x.get_type() == sol::type::function)
            {
                if (y != sol::lua_nil || fn != sol::lua_nil)
                    Fail("with_rotation", "takes (degrees, fn) or (degrees, px, py, fn)");
                DrawTransformed(self, "with_rotation", UITransform::Rotate(angle), x);
                return;
            }
            const UIPoint pivot{ ReadFloat(x, "with_rotation", "px"), ReadFloat(y, "with_rotation", "py") };
            DrawTransformed(self, "with_rotation", UITransform::Rotate(angle, pivot), fn);
        };
        ui["with_transform"] = [](UIScriptFrame &self, const sol::object &transform, const sol::object &fn)
        { DrawTransformed(self, "with_transform", ReadTransform(transform, "with_transform"), fn); };

        // shapes: drawn under the clip, opacity and transform like the widgets, and never taking the pointer
        ui["line"] = [](UIScriptFrame &self, const sol::object &x1, const sol::object &y1, const sol::object &x2,
                         const sol::object &y2, const sol::object &opts)
        {
            UIDrawList &draw = Use(self, "line").DrawList();
            const UIPoint from{ ReadFloat(x1, "line", "x1"), ReadFloat(y1, "line", "y1") };
            const UIPoint to{ ReadFloat(x2, "line", "x2"), ReadFloat(y2, "line", "y2") };
            const ShapeOptions options = ReadShapeOptions(opts, "line", { "color", "width", "dash", "gradient" });
            draw.Line(from, to, options.mColor, options.mStroke, options.mGradient);
        };
        ui["lines"] = [](UIScriptFrame &self, const sol::object &points, const sol::object &opts)
        {
            UIDrawList &draw = Use(self, "lines").DrawList();
            const std::vector<UIPoint> path = ReadPoints(points, "lines", 2);
            const ShapeOptions options =
                ReadShapeOptions(opts, "lines", { "color", "width", "dash", "gradient", "closed" });
            draw.Polyline(path, options.mColor, options.mStroke, options.mClosed, options.mGradient);
        };
        ui["polygon"] = [](UIScriptFrame &self, const sol::object &points, const sol::object &opts)
        {
            UIDrawList &draw = Use(self, "polygon").DrawList();
            const std::vector<UIPoint> shape = ReadPoints(points, "polygon", 3);
            const ShapeOptions options = ReadShapeOptions(opts, "polygon", { "color", "gradient" });
            draw.Polygon(shape, options.mColor, options.mGradient);
        };
        ui["circle"] = [](UIScriptFrame &self, const sol::object &x, const sol::object &y, const sol::object &radius,
                           const sol::object &opts)
        {
            UIDrawList &draw = Use(self, "circle").DrawList();
            const UIPoint center{ ReadFloat(x, "circle", "x"), ReadFloat(y, "circle", "y") };
            const float r = ReadSize(radius, "circle", "radius");
            const ShapeOptions options = ReadShapeOptions(opts, "circle", { "color", "width", "dash", "gradient" });
            if (options.mStroked)
                draw.StrokeCircle(center, r, options.mColor, options.mStroke, options.mGradient);
            else
                draw.FillCircle(center, r, options.mColor, options.mGradient);
        };
        ui["arc"] = [](UIScriptFrame &self, const sol::object &x, const sol::object &y, const sol::object &radius,
                        const sol::object &from, const sol::object &to, const sol::object &opts)
        {
            UIDrawList &draw = Use(self, "arc").DrawList();
            const UIPoint center{ ReadFloat(x, "arc", "x"), ReadFloat(y, "arc", "y") };
            const float r = ReadSize(radius, "arc", "radius");
            const float start = ReadAngle(from, "arc", "from");
            const float end = ReadAngle(to, "arc", "to");
            const ShapeOptions options = ReadShapeOptions(opts, "arc", { "color", "width", "dash", "gradient" });
            draw.Arc(center, r, start, end, options.mColor, options.mStroke, options.mGradient);
        };
        ui["pie"] = [](UIScriptFrame &self, const sol::object &x, const sol::object &y, const sol::object &radius,
                        const sol::object &from, const sol::object &to, const sol::object &opts)
        {
            UIDrawList &draw = Use(self, "pie").DrawList();
            const UIPoint center{ ReadFloat(x, "pie", "x"), ReadFloat(y, "pie", "y") };
            const float r = ReadSize(radius, "pie", "radius");
            const float start = ReadAngle(from, "pie", "from");
            const float end = ReadAngle(to, "pie", "to");
            const ShapeOptions options = ReadShapeOptions(opts, "pie", { "color", "gradient" });
            draw.Pie(center, r, start, end, options.mColor, options.mGradient);
        };
        ui["rect"] = [](UIScriptFrame &self, const sol::object &rect, const sol::object &opts)
        {
            UIDrawList &draw = Use(self, "rect").DrawList();
            const UIRect area = ReadRect(rect, "rect");
            const ShapeOptions options =
                ReadShapeOptions(opts, "rect", { "color", "width", "dash", "gradient", "rounding" });
            if (options.mStroked)
                draw.StrokeRect(area, options.mColor, options.mStroke, options.mRounding, options.mGradient);
            else
                draw.Rect(area, options.mColor, options.mRounding, options.mGradient);
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
            return context.SetFocus(MakeId(self, ReadText(name, "set_focus", "name"),
                index == sol::lua_nil ? 0 : ReadIndex(index, "set_focus", "index")));
        };
        ui["measure_text"] = [](UIScriptFrame &self, const sol::object &text, const sol::object &font,
                                 const sol::object &size)
        {
            const UIContext &context = Use(self, "measure_text");
            const UIStyle &style = context.Style();
            const UISize measured = context.MeasureText(ReadText(text, "measure_text", "text"),
                font == sol::lua_nil ? style.mFont : ReadFont(font, "measure_text", "font"),
                size == sol::lua_nil ? style.mFontSize : ReadSize(size, "measure_text", "size"));
            return std::make_tuple(measured.width, measured.height);
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
            const std::size_t transforms = draw.TransformDepth();
            CallScriptCallback(host, world, instance.mEntity, instance.mRef, "OnUI", args);
            while (draw.ClipDepth() > clips)
                draw.PopClip();
            while (draw.OpacityDepth() > fades)
                draw.PopOpacity();
            while (draw.TransformDepth() > transforms)
                draw.PopTransform();
        }
        frame->mUI = nullptr;
        frame->mOwner = kNullEntity;
    }
}
