/**
 * @file AssetMeta.cpp
 * @author sumin.park
 * @brief The .meta file kept next to a source asset, which holds its guid.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "assets/AssetMeta.h"

#include <core/log/Log.h>
#include <serial/JsonFile.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <format>
#include <random>
#include <system_error>

namespace mir
{
    namespace
    {
        std::optional<uint64_t> ParseGuid(std::string_view text)
        {
            if (text.size() != 16)
                return std::nullopt;
            uint64_t value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
            if (error != std::errc{} || end != text.data() + text.size() || value == 0)
                return std::nullopt;
            return value;
        }

        std::string Lower(const std::filesystem::path &path)
        {
            std::string name = path.filename().string();
            std::transform(name.begin(), name.end(), name.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return name;
        }

        bool EndsWith(const std::string &text, std::string_view suffix)
        {
            return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
        }
    }

    AssetId MakeChildAssetId(AssetId parent, std::string_view key)
    {
        const uint64_t value = Fnv1a64(std::format("{:016x}#{}", parent.value, key));
        return AssetId{ value != 0 ? value : 1 };
    }

    std::filesystem::path AssetMetaPath(const std::filesystem::path &source)
    {
        return std::filesystem::path(source.string() + std::string(kAssetMetaExtension));
    }

    AssetId MakeRandomGuid(const std::unordered_set<uint64_t> &taken)
    {
        std::random_device device;
        std::mt19937_64 generator((static_cast<uint64_t>(device()) << 32) ^ device());
        for (;;)
        {
            const uint64_t value = generator();
            if (value != 0 && !taken.contains(value))
                return AssetId{ value };
        }
    }

    std::optional<AssetMeta> ReadAssetMeta(const std::filesystem::path &metaPath)
    {
        std::error_code ec;
        if (!std::filesystem::exists(metaPath, ec))
            return std::nullopt;

        std::string error;
        const std::optional<nlohmann::json> root = ReadJsonFile(metaPath, &error);
        if (!root || !root->is_object())
        {
            MIR_LOG_ERROR("{}: not a valid .meta file{}", metaPath.string(), error.empty() ? "" : ": " + error);
            return std::nullopt;
        }

        const auto version = root->find("version");
        if (version == root->end() || !version->is_number_unsigned() || version->get<uint32_t>() != kAssetMetaVersion)
        {
            MIR_LOG_ERROR("{}: unsupported .meta version, this build reads version {}", metaPath.string(),
                kAssetMetaVersion);
            return std::nullopt;
        }

        const auto guid = root->find("guid");
        const std::optional<uint64_t> value =
            guid != root->end() && guid->is_string() ? ParseGuid(guid->get<std::string>()) : std::nullopt;
        if (!value)
        {
            MIR_LOG_ERROR("{}: the guid must be 16 hex digits and not zero", metaPath.string());
            return std::nullopt;
        }

        AssetMeta meta;
        meta.guid = AssetId{ *value };
        if (const auto importer = root->find("importer"); importer != root->end())
        {
            if (!importer->is_object())
            {
                MIR_LOG_ERROR("{}: importer must be an object", metaPath.string());
                return std::nullopt;
            }
            meta.importer = importer->dump();
        }
        return meta;
    }

    bool WriteAssetMeta(const std::filesystem::path &metaPath, const AssetMeta &meta)
    {
        const nlohmann::json importer = nlohmann::json::parse(meta.importer, nullptr, false);
        if (meta.guid.IsNull() || !importer.is_object())
        {
            MIR_LOG_ERROR("{}: cannot write a .meta with a null guid or an importer that is not a JSON object",
                metaPath.string());
            return false;
        }
        const nlohmann::json root{ { "version", kAssetMetaVersion },
            { "guid", std::format("{:016x}", meta.guid.value) },{ "importer", importer } };
        return WriteJsonFile(metaPath, root);
    }

    void MoveAssetMeta(const std::filesystem::path &from, const std::filesystem::path &to)
    {
        std::error_code ec;
        const std::filesystem::path meta = AssetMetaPath(from);
        if (!std::filesystem::exists(meta, ec))
            return;
        // a .meta already at the destination belongs to a file that is gone
        std::filesystem::rename(meta, AssetMetaPath(to), ec);
        if (ec)
            MIR_LOG_ERROR("could not move {} to {}: {}", meta.string(), AssetMetaPath(to).string(), ec.message());
    }

    void RemoveAssetMeta(const std::filesystem::path &source)
    {
        std::error_code ec;
        std::filesystem::remove(AssetMetaPath(source), ec);
    }

    bool IsAssetToolingFile(const std::filesystem::path &path)
    {
        const std::string name = Lower(path);
        return EndsWith(name, kAssetMetaExtension) || EndsWith(name, ".import.json") || EndsWith(name, ".pending") ||
            name == ".gitkeep";
    }
}
