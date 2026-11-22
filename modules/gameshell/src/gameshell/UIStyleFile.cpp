/**
 * @file UIStyleFile.cpp
 * @author Quy Dong Ta
 * @brief A game's UI style file: a JSON object of UIStyle fields by name, read
 *        over a base style, with its sprites and font loaded by the caller.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameshell/UIStyleFile.h>

#include <gameui/UIStyleFields.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <format>
#include <optional>
#include <variant>

namespace mir
{
    namespace
    {
        using Json = nlohmann::json;

        // why the value is wrong, empty when it is read
        using Problem = std::string;

        std::optional<float> Number(const Json &value)
        {
            if (!value.is_number())
                return std::nullopt;
            const float number = value.get<float>();
            return std::isfinite(number) ? std::optional<float>(number) : std::nullopt;
        }

        Problem Read(const Json &value, const UIStyleField &, UIColor &out, const UIStyleAssets &)
        {
            if (!value.is_array() || value.size() < 3 || value.size() > 4)
                return "must be [r, g, b] or [r, g, b, a]";
            float channels[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            for (std::size_t i = 0; i < value.size(); ++i)
            {
                const std::optional<float> channel = Number(value[i]);
                if (!channel)
                    return "must be [r, g, b] or [r, g, b, a] of numbers";
                channels[i] = *channel;
            }
            out = UIColor{channels[0], channels[1], channels[2], channels[3]};
            return {};
        }

        Problem Read(const Json &value, const UIStyleField &field, float &out, const UIStyleAssets &)
        {
            const std::optional<float> number = Number(value);
            if (!number)
                return "must be a number";
            if (!field.mSigned && *number < 0.0f)
                return "must be 0 or more";
            out = *number;
            return {};
        }

        Problem Read(const Json &value, const UIStyleField &, UITextAlign &out, const UIStyleAssets &)
        {
            const std::optional<UITextAlign> align =
                value.is_string() ? ParseUITextAlign(value.get<std::string>()) : std::nullopt;
            if (!align)
                return "must be \"left\", \"center\" or \"right\"";
            out = *align;
            return {};
        }

        // null is the backend's own font
        Problem Read(const Json &value, const UIStyleField &, UIFont &out, const UIStyleAssets &assets)
        {
            if (value.is_null())
            {
                out = UIFont{};
                return {};
            }
            if (!value.is_string())
                return "must be a font's path or null";
            const std::string path = value.get<std::string>();
            const UIFont font = assets.mFont ? assets.mFont(path) : UIFont{};
            if (!font.IsValid())
                return std::format("'{}' did not load as a font", path);
            out = font;
            return {};
        }

        // "path", { "path": ..., "slice": 8 or [l, t, r, b] }, or null for none
        Problem Read(const Json &value, const UIStyleField &, UISprite &out, const UIStyleAssets &assets)
        {
            if (value.is_null())
            {
                out = UISprite{};
                return {};
            }
            constexpr std::string_view kShape = "must be a path, { \"path\", \"slice\" } or null";
            const Json *path = &value;
            UIEdges slice;
            if (value.is_object())
            {
                for (const auto &[key, entry] : value.items())
                    if (key != "path" && key != "slice")
                        return std::format("has an unknown key '{}'; valid ones are path, slice", key);
                const auto found = value.find("path");
                if (found == value.end())
                    return std::string(kShape);
                path = &*found;
                if (const auto sliceValue = value.find("slice"); sliceValue != value.end())
                {
                    float edges[4] = {};
                    if (const std::optional<float> all = Number(*sliceValue))
                        edges[0] = edges[1] = edges[2] = edges[3] = *all;
                    else if (sliceValue->is_array() && sliceValue->size() == 4)
                    {
                        for (std::size_t i = 0; i < 4; ++i)
                        {
                            const std::optional<float> edge = Number((*sliceValue)[i]);
                            if (!edge)
                                return "slice must be a number or [left, top, right, bottom]";
                            edges[i] = *edge;
                        }
                    }
                    else
                        return "slice must be a number or [left, top, right, bottom]";
                    for (const float edge : edges)
                        if (edge < 0.0f)
                            return "slice must be 0 or more";
                    slice = UIEdges{edges[0], edges[1], edges[2], edges[3]};
                }
            }
            if (!path->is_string())
                return std::string(kShape);
            const std::string file = path->get<std::string>();
            UISprite sprite = assets.mSprite ? assets.mSprite(file) : UISprite{};
            if (!sprite.IsValid())
                return std::format("'{}' did not load as a texture", file);
            sprite.mSlice = slice;
            out = sprite;
            return {};
        }
    } // namespace

    UIStyle ParseUIStyle(std::string_view text, const UIStyle &base, const UIStyleAssets &assets,
                         std::vector<std::string> &errors)
    {
        Json json;
        try
        {
            json = Json::parse(text);
        }
        catch (const Json::exception &error)
        {
            errors.push_back(std::format("not valid JSON: {}", error.what()));
            return base;
        }
        if (!json.is_object())
        {
            errors.push_back("not a JSON object of style fields");
            return base;
        }

        UIStyle style = base;
        for (const auto &[key, value] : json.items())
        {
            const UIStyleField *field = FindUIStyleField(key);
            if (field == nullptr)
            {
                errors.push_back(std::format("unknown key '{}'; valid ones are {}", key, UIStyleFieldNames()));
                continue;
            }
            const Problem problem = std::visit(
                [&](auto member) { return Read(value, *field, style.*member, assets); }, field->mMember);
            if (!problem.empty())
                errors.push_back(std::format("{} {}", key, problem));
        }
        return style;
    }
} // namespace mir
