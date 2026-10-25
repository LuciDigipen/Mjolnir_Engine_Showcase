/**
 * @file PhysicsLayerSettings.cpp
 * @author sumin.park
 * @brief project_settings.json's "physicsLayers" section.
 *
 * Shape: { "names": ["Default", "Player", "", "Enemy"], "ignore": [["Player", "Player"]] }. A name may be empty,
 * so a removed layer keeps its slot and no index shifts; "ignore" lists the pairs that do NOT collide.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/PhysicsLayerSettings.h>

#include <core/log/SetupError.h>
#include <serial/JsonFile.h>

#include <nlohmann/json.hpp>

#include <set>
#include <vector>

namespace mir
{
    namespace
    {
        using json = nlohmann::json;

        constexpr const char *kSection = "physicsLayers";

        [[noreturn]] void Invalid(const std::filesystem::path &path, const std::string &what)
        {
            throw SetupError(ErrorCategory::ProjectData, "project_settings.json has an invalid physicsLayers section",
                path, what,
                "Fix the physicsLayers section in " + path.string() + ", or remove it to put every layer back to Default.");
        }

        PhysicsLayerTable Parse(const json &section, const std::filesystem::path &path)
        {
            if (!section.is_object())
                Invalid(path, "physicsLayers is not an object");

            std::vector<std::string> names;
            if (const auto found = section.find("names"); found != section.end())
            {
                if (!found->is_array())
                    Invalid(path, "names is not an array");
                for (const json &name : *found)
                {
                    if (!name.is_string())
                        Invalid(path, "a layer name is not a string");
                    names.push_back(name.get<std::string>());
                }
            }

            // Default is layer 0; it is added when the list does not start with it
            if (names.empty() || names[0] != "Default")
            {
                for (const std::string &name : names)
                {
                    if (name == "Default")
                        Invalid(path, "Default must be the first layer");
                }
                names.insert(names.begin(), "Default");
            }
            if (static_cast<int>(names.size()) > kPhysicsLayerCount)
                Invalid(path, "there are " + std::to_string(names.size()) + " layers, the most is "
                    + std::to_string(kPhysicsLayerCount));

            PhysicsLayerTable table;
            std::set<std::string> seen;
            for (size_t i = 0; i < names.size(); ++i)
            {
                if (!names[i].empty() && !seen.insert(names[i]).second)
                    Invalid(path, "the layer name '" + names[i] + "' is used twice");
                table.names[i] = names[i];
            }

            if (const auto found = section.find("ignore"); found != section.end())
            {
                if (!found->is_array())
                    Invalid(path, "ignore is not an array");
                for (const json &pair : *found)
                {
                    if (!pair.is_array() || pair.size() != 2 || !pair[0].is_string() || !pair[1].is_string())
                        Invalid(path, "an ignore entry is not a pair of layer names");
                    const std::optional<int> a = table.IndexOf(pair[0].get<std::string>());
                    const std::optional<int> b = table.IndexOf(pair[1].get<std::string>());
                    if (!a || !b)
                        Invalid(path, "ignore names the layer '" + pair[a ? 1 : 0].get<std::string>()
                            + "', which does not exist");
                    table.SetCollide(*a, *b, false);
                }
            }
            return table;
        }
    }

    PhysicsLayerTable ReadPhysicsLayers(const std::filesystem::path &settingsPath)
    {
        const std::optional<json> root = ReadJsonFile(settingsPath);
        if (!root || !root->is_object())
            return {};

        const auto found = root->find(kSection);
        if (found == root->end())
            return {};
        return Parse(*found, settingsPath);
    }

    bool WritePhysicsLayers(const std::filesystem::path &settingsPath, const PhysicsLayerTable &table)
    {
        json names = json::array();
        int last = 0;
        for (int i = 0; i < kPhysicsLayerCount; ++i)
        {
            if (!table.names[i].empty())
                last = i;
        }
        for (int i = 0; i <= last; ++i)
            names.push_back(table.names[i]);

        json ignore = json::array();
        for (int a = 0; a <= last; ++a)
        {
            for (int b = a; b <= last; ++b)
            {
                if (!table.names[a].empty() && !table.names[b].empty() && !table.Collide(a, b))
                    ignore.push_back(json::array({ table.names[a], table.names[b] }));
            }
        }

        return UpdateJsonFile(settingsPath,
            [&](json &root) { root[kSection] = json{ { "names", names }, { "ignore", ignore } }; });
    }

    std::string CheckPhysicsLayers(const PhysicsLayerTable &table)
    {
        if (table.names[0] != "Default")
            return "layer 0 must stay named Default";

        std::set<std::string> seen;
        for (const std::string &name : table.names)
        {
            if (!name.empty() && !seen.insert(name).second)
                return "the layer name '" + name + "' is used twice";
        }
        return {};
    }
}
