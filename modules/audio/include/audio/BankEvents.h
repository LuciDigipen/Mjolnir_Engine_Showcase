/**
 * @file BankEvents.h
 * @author Quy Dong Ta
 * @brief The events each SoundBank holds, read from the metadata Wwise writes beside it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mir
{
    struct BankEvents
    {
        std::string mBank;
        std::vector<std::string> mEvents;
        bool mListed = false; // false = no readable bank.json beside the bank

        bool operator==(const BankEvents &) const = default;
    };

    // Every bank in bankDir except Init with its events sorted. Reads files only, no sound engine
    std::vector<BankEvents> ListBankEvents(const std::filesystem::path &bankDir);
}
