/**
 * @file IdMigration.cpp
 * @author sumin.park
 * @brief One-off move of model ids from the old import.json files to .meta guids.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "IdMigration.h"

#include <assets/AssetId.h>
#include <assets/AssetMeta.h>
#include <core/log/Log.h>

#include <nlohmann/json.hpp>

#include <charconv>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mir::cook
{
    namespace
    {
        using Json = nlohmann::json;
        constexpr std::string_view kOldSidecar = ".import.json";

        bool IsOldSidecar(const std::filesystem::path &path)
        {
            const std::string name = path.filename().string();
            return name.size() > kOldSidecar.size() && name.ends_with(kOldSidecar);
        }

        Json ReadJson(const std::filesystem::path &path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                throw std::runtime_error("cannot read " + path.string());
            Json json = Json::parse(in, nullptr, false);
            if (json.is_discarded())
                throw std::runtime_error(path.string() + " is not valid JSON");
            return json;
        }

        std::optional<uint64_t> ParseId(const std::string &text)
        {
            if (text.size() != 16)
                return std::nullopt;
            uint64_t value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
            if (error != std::errc{} || end != text.data() + text.size())
                return std::nullopt;
            return value;
        }

        // Replaces each string that is an old id, and counts the ones it replaced
        void Rewrite(Json &node, const std::unordered_map<uint64_t, uint64_t> &remap, bool apply, int &count)
        {
            if (node.is_string())
            {
                const std::optional<uint64_t> id = ParseId(node.get<std::string>());
                if (!id)
                    return;
                const auto found = remap.find(*id);
                if (found == remap.end())
                    return;
                ++count;
                if (apply)
                    node = std::format("{:016x}", found->second);
            }
            else if (node.is_array() || node.is_object())
                for (auto &child : node)
                    Rewrite(child, remap, apply, count);
        }
    }

    void MigrateIds(const IdMigrationOptions &options)
    {
        SourceAssetIds ids(options.roots);
        ids.Scan();

        // old id to new id, a model's alias and part ids included
        std::unordered_map<uint64_t, uint64_t> remap;
        std::vector<std::filesystem::path> sidecars;
        for (const AssetSourceRoot &root : options.roots)
            for (const auto &entry : std::filesystem::recursive_directory_iterator(root.physical))
                if (entry.is_regular_file() && IsOldSidecar(entry.path()))
                    sidecars.push_back(entry.path());

        for (const std::filesystem::path &sidecar : sidecars)
        {
            std::filesystem::path source = sidecar;
            source.replace_extension();
            source.replace_extension();
            if (!std::filesystem::exists(source))
            {
                MIR_LOG_WARN("{} has no source file, so it is left alone", sidecar.string());
                continue;
            }

            const Json old = ReadJson(sidecar);
            if (!old.contains("identityPath") || !old.at("identityPath").is_string() || !old.contains("objects") ||
                !old.at("objects").is_object())
                throw std::runtime_error(sidecar.string() + " is not an import.json this tool reads");
            const std::string identity = old.at("identityPath").get<std::string>();

            // a dry run has no guid to give, so the new ids are only counted
            const AssetId guid = options.apply ? ids.Meta(source).guid : AssetId{ 1 };
            remap[MakeAssetId(identity).value] = guid.value;
            if (old.contains("aliases"))
                for (const Json &alias : old.at("aliases"))
                    remap[MakeAssetId(alias.get<std::string>()).value] = guid.value;

            Json objects = Json::object();
            for (const auto &[match, number] : old.at("objects").items())
            {
                const std::string kind = match.substr(0, match.find(':'));
                const std::string key = kind + "/" + (number.is_string() ? number.get<std::string>() : number.dump());
                remap[MakeAssetId(identity + "#" + key).value] = MakeChildAssetId(guid, key).value;
                objects[match] = number;
            }

            MIR_LOG_INFO("{}: {} part ids {}", source.string(), old.at("objects").size(),
                options.apply ? "carried into its .meta" : "would be carried into a .meta");
            if (options.apply)
            {
                ids.Meta(source).importer = Json{ { "next", old.value("next", 1) }, { "objects", objects } }.dump();
                ids.Save(source);
                std::filesystem::remove(sidecar);
            }
        }

        int total = 0;
        for (const std::filesystem::path &dir : options.dataDirs)
            for (const auto &entry : std::filesystem::recursive_directory_iterator(dir))
            {
                const std::string extension = entry.path().extension().string();
                if (!entry.is_regular_file() || (extension != ".json" && extension != ".prefab"))
                    continue;
                Json data = ReadJson(entry.path());
                int count = 0;
                Rewrite(data, remap, options.apply, count);
                if (count == 0)
                    continue;
                total += count;
                MIR_LOG_INFO("{}: {} ids {}", entry.path().string(), count, options.apply ? "rewritten" : "would be rewritten");
                if (options.apply)
                    std::ofstream(entry.path(), std::ios::binary | std::ios::trunc) << data.dump(2) << "\n";
            }

        MIR_LOG_INFO("{} models with an import.json, {} ids {}{}", sidecars.size(), total,
            options.apply ? "rewritten" : "would be rewritten", options.apply ? "" : ", run again with --apply");
    }
}
