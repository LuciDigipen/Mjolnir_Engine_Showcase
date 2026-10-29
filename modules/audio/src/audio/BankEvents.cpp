/**
 * @file BankEvents.cpp
 * @author Quy Dong Ta
 * @brief The events each SoundBank holds, read from the metadata Wwise writes beside it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <audio/BankEvents.h>

#include <core/fs/FileIO.h>
#include <core/log/Log.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <system_error>

namespace mir
{
    namespace
    {
        using json = nlohmann::json;

        std::optional<std::vector<std::string>> ReadEvents(const std::filesystem::path &path)
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec))
                return std::nullopt;

            const std::optional<std::string> text = ReadText(path);
            const json root = text ? json::parse(*text, nullptr, false) : json();

            static const json::json_pointer kBanks("/SoundBanksInfo/SoundBanks");
            if (root.is_discarded() || !root.is_object() || !root.contains(kBanks) || !root.at(kBanks).is_array())
            {
                MIR_LOG_WARN("SoundBank metadata '{}' is not Wwise's JSON metadata; its events are not listed",
                    path.string());
                return std::nullopt;
            }

            std::vector<std::string> events;
            for (const json &bank : root.at(kBanks))
            {
                const auto list = bank.find("Events");
                if (list == bank.end() || !list->is_array())
                    continue;

                for (const json &event : *list)
                {
                    const auto name = event.find("Name");
                    if (name != event.end() && name->is_string())
                        events.push_back(name->get<std::string>());
                }
            }

            std::sort(events.begin(), events.end());
            events.erase(std::unique(events.begin(), events.end()), events.end());
            return events;
        }
    }

    std::vector<BankEvents> ListBankEvents(const std::filesystem::path &bankDir)
    {
        std::vector<BankEvents> banks;

        std::error_code ec;
        for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(bankDir, ec))
        {
            const std::filesystem::path &path = entry.path();
            std::error_code entryEc;
            if (!entry.is_regular_file(entryEc) || path.extension() != ".bnk" || path.filename() == "Init.bnk")
                continue;

            const std::optional<std::vector<std::string>> events =
                ReadEvents(std::filesystem::path(path).replace_extension(".json"));
            banks.push_back({path.stem().string(), events.value_or(std::vector<std::string>{}), events.has_value()});
        }

        std::sort(banks.begin(), banks.end(),
            [](const BankEvents &a, const BankEvents &b) { return a.mBank < b.mBank; });
        return banks;
    }
}
