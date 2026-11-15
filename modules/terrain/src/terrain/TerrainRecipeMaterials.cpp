/**
 * @file TerrainRecipeMaterials.cpp
 * @author Jackey Cen
 * @brief A recipe's material layers: JSON rules, their diagnostics and the on-disk form.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain/TerrainRecipeMaterials.h>

#include <terrain/TerrainWeights.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <set>
#include <utility>

namespace mir
{
    namespace
    {
        static_assert(kMaxMaterialNameLength == kMaxTerrainMaterialLayerName);

        using Json = nlohmann::json;

        std::string JoinPath(std::string_view parent, std::string_view key)
        {
            std::string path(parent);
            if (!path.empty())
                path += '.';
            path += key;
            return path;
        }

        std::string FormatNumber(double value)
        {
            if (std::isnan(value))
                return "nan";
            if (std::isinf(value))
                return value < 0.0 ? "-inf" : "inf";
            char buffer[64];
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
            return std::string(buffer, result.ptr);
        }

        // Floats keep a ".0" so 1.0 does not read like an integer in messages.
        std::string Describe(const Json &value)
        {
            if (value.is_number_float())
            {
                std::string text = FormatNumber(value.get<double>());
                if (text.find_first_of(".eni") == std::string::npos)
                    text += ".0";
                return text;
            }
            std::string text = value.dump();
            constexpr size_t kMaxLength = 64;
            if (text.size() > kMaxLength)
                text = text.substr(0, kMaxLength) + "...";
            return text;
        }

        class IssueChecker
        {
          public:
            void Add(std::string path, std::string message)
            {
                mIssues.push_back({std::move(path), std::move(message)});
            }
            std::vector<TerrainRecipeMaterialIssue> Take()
            {
                return std::move(mIssues);
            }

          private:
            std::vector<TerrainRecipeMaterialIssue> mIssues;
        };

        // A missing or mistyped object is reported once; its fields then read as absent.
        class ObjectReader
        {
          public:
            ObjectReader(IssueChecker &checker, const Json *object, std::string path,
                         std::initializer_list<std::string_view> keys)
                : mChecker(checker), mPath(std::move(path))
            {
                if (!object)
                    return;
                if (!object->is_object())
                {
                    mChecker.Add(mPath, "expected an object, got " + Describe(*object));
                    return;
                }
                mObject = object;
                for (const auto &[key, value] : object->items())
                    if (std::find(keys.begin(), keys.end(), key) == keys.end())
                        mChecker.Add(Path(key), "unknown field (value " + Describe(value) + ")");
            }

            std::string Path(std::string_view key) const
            {
                return JoinPath(mPath, key);
            }

            const Json *Field(std::string_view key) const
            {
                if (!mObject)
                    return nullptr;
                const auto it = mObject->find(key);
                if (it == mObject->end())
                {
                    mChecker.Add(Path(key), "missing required field");
                    return nullptr;
                }
                return &*it;
            }

            // Null when absent, without reporting it.
            const Json *OptionalField(std::string_view key) const
            {
                if (!mObject)
                    return nullptr;
                const auto it = mObject->find(key);
                return it == mObject->end() ? nullptr : &*it;
            }

            // An integer must convert exactly; -0 reads as +0 so it cannot split a fingerprint.
            std::optional<double> FiniteNumber(std::string_view key) const
            {
                const Json *value = Field(key);
                if (!value)
                    return std::nullopt;
                double number = 0.0;
                if (value->is_number_float())
                {
                    number = value->get<double>();
                    if (!std::isfinite(number))
                    {
                        mChecker.Add(Path(key), Describe(*value) + " is not a finite number");
                        return std::nullopt;
                    }
                }
                else if (value->is_number_unsigned())
                {
                    const uint64_t integer = value->get<uint64_t>();
                    number = static_cast<double>(integer);
                    if (number >= 18446744073709551616.0 || static_cast<uint64_t>(number) != integer)
                    {
                        mChecker.Add(Path(key), Describe(*value) + " is not exactly representable as a double");
                        return std::nullopt;
                    }
                }
                else if (value->is_number_integer())
                {
                    const int64_t integer = value->get<int64_t>();
                    number = static_cast<double>(integer);
                    if (number < -9223372036854775808.0 || static_cast<int64_t>(number) != integer)
                    {
                        mChecker.Add(Path(key), Describe(*value) + " is not exactly representable as a double");
                        return std::nullopt;
                    }
                }
                else
                {
                    mChecker.Add(Path(key), "expected a number, got " + Describe(*value));
                    return std::nullopt;
                }
                return number == 0.0 ? 0.0 : number;
            }

            std::optional<double> Number(std::string_view key, double low, double high, std::string_view unit) const
            {
                const std::optional<double> number = FiniteNumber(key);
                if (number && (*number < low || *number > high))
                {
                    mChecker.Add(Path(key), FormatNumber(*number) + " is out of range [" + FormatNumber(low) + ", " +
                                                FormatNumber(high) + "]" + (unit.empty() ? "" : " ") +
                                                std::string(unit));
                    return std::nullopt;
                }
                return number;
            }

            std::optional<int64_t> Integer(std::string_view key, int64_t low, int64_t high) const
            {
                const Json *value = Field(key);
                if (!value)
                    return std::nullopt;
                if (!value->is_number_integer())
                {
                    mChecker.Add(Path(key), "expected an integer, got " + Describe(*value));
                    return std::nullopt;
                }
                // Integers past int64 only fit the unsigned type and are out of any range used here.
                const bool fits = !value->is_number_unsigned() ||
                                  value->get<uint64_t>() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
                const int64_t integer = fits ? value->get<int64_t>() : std::numeric_limits<int64_t>::max();
                if (!fits || integer < low || integer > high)
                {
                    mChecker.Add(Path(key), Describe(*value) + " is out of range [" + std::to_string(low) + ", " +
                                                std::to_string(high) + "]");
                    return std::nullopt;
                }
                return integer;
            }

            std::optional<std::string> String(std::string_view key) const
            {
                const Json *value = Field(key);
                if (!value)
                    return std::nullopt;
                if (!value->is_string())
                {
                    mChecker.Add(Path(key), "expected a string, got " + Describe(*value));
                    return std::nullopt;
                }
                return value->get<std::string>();
            }

          private:
            IssueChecker &mChecker;
            const Json *mObject = nullptr;
            std::string mPath;
        };

        std::optional<TerrainMaterialHeightRule> ReadHeightRule(IssueChecker &checker, const Json *json,
                                                                const std::string &path)
        {
            const ObjectReader rule(checker, json, path, {"min", "max", "blend"});
            const auto min = rule.Number("min", -kMaxTerrainHeightMagnitude, kMaxTerrainHeightMagnitude, "m");
            const auto max = rule.Number("max", -kMaxTerrainHeightMagnitude, kMaxTerrainHeightMagnitude, "m");
            const std::optional<double> blend = rule.FiniteNumber("blend");
            if (blend && *blend < 0.0)
                checker.Add(path + ".blend", FormatNumber(*blend) + " must be greater than or equal to 0");
            if (min && max && *min > *max)
                checker.Add(path,
                            "min " + FormatNumber(*min) + " must be less than or equal to max " + FormatNumber(*max));
            if (!min || !max || !blend || *blend < 0.0)
                return std::nullopt;
            return TerrainMaterialHeightRule{*min, *max, *blend};
        }

        std::optional<TerrainMaterialSlopeRule> ReadSlopeRule(IssueChecker &checker, const Json *json,
                                                              const std::string &path)
        {
            const ObjectReader rule(checker, json, path, {"min", "max", "blend"});
            const auto min = rule.Number("min", 0.0, kMaxTerrainMaterialSlopeDegrees, "deg");
            const auto max = rule.Number("max", 0.0, kMaxTerrainMaterialSlopeDegrees, "deg");
            const std::optional<double> blend = rule.FiniteNumber("blend");
            if (blend && *blend < 0.0)
                checker.Add(path + ".blend", FormatNumber(*blend) + " must be greater than or equal to 0");
            if (min && max && *min > *max)
                checker.Add(path,
                            "min " + FormatNumber(*min) + " must be less than or equal to max " + FormatNumber(*max));
            if (!min || !max || !blend || *blend < 0.0)
                return std::nullopt;
            return TerrainMaterialSlopeRule{*min, *max, *blend};
        }

        std::optional<TerrainMaterialNoiseRule> ReadNoiseRule(IssueChecker &checker, const Json *json,
                                                              const std::string &path)
        {
            const ObjectReader rule(checker, json, path, {"wavelength", "octaves", "strength"});
            const auto wavelength =
                rule.Number("wavelength", kMinTerrainNoiseWavelength, kMaxTerrainNoiseWavelength, "m");
            const auto octaves = rule.Integer("octaves", 1, kMaxTerrainMaterialNoiseOctaves);
            const auto strength = rule.Number("strength", 0.0, 1.0, "");
            if (!wavelength || !octaves || !strength)
                return std::nullopt;
            return TerrainMaterialNoiseRule{*wavelength, static_cast<int32_t>(*octaves), *strength};
        }

        std::optional<TerrainMaterialRules> ReadMaterialRules(IssueChecker &checker, const Json *json,
                                                              const std::string &path)
        {
            const ObjectReader rule(checker, json, path, {"height", "slope", "noise"});
            TerrainMaterialRules rules;
            bool complete = true;
            if (const Json *height = rule.OptionalField("height"))
            {
                rules.height = ReadHeightRule(checker, height, rule.Path("height"));
                complete = complete && rules.height.has_value();
            }
            if (const Json *slope = rule.OptionalField("slope"))
            {
                rules.slope = ReadSlopeRule(checker, slope, rule.Path("slope"));
                complete = complete && rules.slope.has_value();
            }
            if (const Json *noise = rule.OptionalField("noise"))
            {
                rules.noise = ReadNoiseRule(checker, noise, rule.Path("noise"));
                complete = complete && rules.noise.has_value();
            }
            return complete ? std::optional<TerrainMaterialRules>(std::move(rules)) : std::nullopt;
        }

        std::vector<TerrainRecipeMaterialLayer> ReadMaterials(IssueChecker &checker, const Json &materials)
        {
            if (!materials.is_object())
            {
                checker.Add("materials", "expected an object, got " + Describe(materials));
                return {};
            }

            const ObjectReader reader(checker, &materials, "materials", {"layers"});
            const Json *array = reader.OptionalField("layers");
            if (!array)
            {
                checker.Add("materials.layers", "missing required field");
                return {};
            }
            if (!array->is_array())
            {
                checker.Add("materials.layers", "expected an array, got " + Describe(*array));
                return {};
            }
            if (array->empty())
            {
                checker.Add("materials.layers", "at least one layer is required");
                return {};
            }
            if (array->size() > size_t(kMaxTerrainMaterialLayers))
                checker.Add("materials.layers", std::to_string(array->size()) + " layers exceed the limit " +
                                                    std::to_string(kMaxTerrainMaterialLayers));

            std::vector<TerrainRecipeMaterialLayer> layers;
            std::set<std::string> names;
            for (size_t i = 0; i < array->size(); ++i)
            {
                const std::string path = "materials.layers[" + std::to_string(i) + "]";
                const ObjectReader layer(checker, &(*array)[i], path, {"name", "rules"});
                const std::optional<std::string> name = layer.String("name");
                if (name)
                {
                    if (!IsValidTerrainMaterialName(*name))
                        checker.Add(path + ".name",
                                    "\"" + *name + "\" must be 1 to " + std::to_string(kMaxMaterialNameLength) +
                                        " characters of [a-z0-9_]");
                    else if (!names.insert(*name).second)
                        checker.Add(path + ".name", "duplicate layer name \"" + *name + "\"");
                }

                TerrainMaterialRules rules;
                if (const Json *rulesJson = layer.OptionalField("rules"))
                {
                    if (i == 0)
                        checker.Add(path + ".rules", "the first layer is the fallback and takes no rules");
                    else if (std::optional<TerrainMaterialRules> parsed =
                                 ReadMaterialRules(checker, rulesJson, layer.Path("rules")))
                        rules = std::move(*parsed);
                }
                if (name)
                    layers.push_back({*name, std::move(rules)});
            }
            return layers;
        }
    } // namespace

    bool IsValidTerrainMaterialName(std::string_view name)
    {
        return IsValidTerrainMaterialLayerName(name);
    }

    std::vector<TerrainRecipeMaterialLayer> DefaultTerrainMaterialLayers()
    {
        return {TerrainRecipeMaterialLayer{std::string("base"), {}}};
    }

    TerrainRecipeMaterialsParseResult ParseTerrainRecipeMaterials(const nlohmann::json *materials)
    {
        if (!materials)
            return {DefaultTerrainMaterialLayers(), {}};
        IssueChecker checker;
        std::vector<TerrainRecipeMaterialLayer> layers = ReadMaterials(checker, *materials);
        return {std::move(layers), checker.Take()};
    }

    nlohmann::ordered_json TerrainRecipeMaterialsToJson(std::span<const TerrainRecipeMaterialLayer> layers)
    {
        nlohmann::ordered_json array = nlohmann::ordered_json::array();
        for (const TerrainRecipeMaterialLayer &layer : layers)
        {
            nlohmann::ordered_json json = {{"name", layer.name}};
            nlohmann::ordered_json rules = nlohmann::ordered_json::object();
            if (layer.rules.height)
                rules["height"] = {{"min", layer.rules.height->min},
                                   {"max", layer.rules.height->max},
                                   {"blend", layer.rules.height->blend}};
            if (layer.rules.slope)
                rules["slope"] = {{"min", layer.rules.slope->min},
                                  {"max", layer.rules.slope->max},
                                  {"blend", layer.rules.slope->blend}};
            if (layer.rules.noise)
                rules["noise"] = {{"wavelength", layer.rules.noise->wavelength},
                                  {"octaves", layer.rules.noise->octaves},
                                  {"strength", layer.rules.noise->strength}};
            if (!rules.empty())
                json["rules"] = std::move(rules);
            array.push_back(std::move(json));
        }
        return {{"layers", std::move(array)}};
    }

    TerrainRecipeMaterialsWriteResult ReplaceTerrainRecipeMaterials(std::string_view recipeText,
                                                                    std::span<const TerrainRecipeMaterialLayer> layers)
    {
        const nlohmann::ordered_json materials = TerrainRecipeMaterialsToJson(layers);
        const nlohmann::json plain = materials;
        const TerrainRecipeMaterialsParseResult checked = ParseTerrainRecipeMaterials(&plain);
        if (!checked.issues.empty())
        {
            const TerrainRecipeMaterialIssue &first = checked.issues.front();
            return {std::nullopt,
                    first.path.empty() ? first.message : first.path + ": " + first.message};
        }

        nlohmann::ordered_json root;
        try
        {
            root = nlohmann::ordered_json::parse(recipeText);
        }
        catch (const nlohmann::ordered_json::exception &error)
        {
            return {std::nullopt, std::string("not valid JSON: ") + error.what()};
        }
        if (!root.is_object())
            return {std::nullopt, "the recipe must be a JSON object"};

        if (root.contains("materials"))
            root["materials"] = materials;
        else
        {
            nlohmann::ordered_json rebuilt = nlohmann::ordered_json::object();
            for (auto it = root.begin(); it != root.end(); ++it)
            {
                rebuilt[it.key()] = it.value();
                if (it.key() == "parameters")
                    rebuilt["materials"] = materials;
            }
            if (!rebuilt.contains("materials"))
                rebuilt["materials"] = materials;
            root = std::move(rebuilt);
        }
        return {root.dump(2) + "\n", {}};
    }
} // namespace mir
