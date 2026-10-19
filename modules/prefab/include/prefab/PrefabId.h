/**
 * @file PrefabId.h
 * @author Jackey Cen
 * @brief Persistent 128-bit prefab identity (UUID v4), path-independent.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace mir
{
    struct PrefabId
    {
        uint64_t mHigh = 0;
        uint64_t mLow = 0;

        constexpr bool IsNull() const { return mHigh == 0 && mLow == 0; }
        constexpr bool operator==(const PrefabId &) const = default;
        constexpr auto operator<=>(const PrefabId &) const = default;
    };

    inline constexpr PrefabId kNullPrefabId{};

    PrefabId GeneratePrefabId();

    // Canonical lowercase 8-4-4-4-12 hex; parsing accepts upper or lower case
    // in exactly that layout and rejects the nil UUID.
    std::string ToString(PrefabId id);
    std::optional<PrefabId> ParsePrefabId(std::string_view text);
}

namespace std
{
    template <>
    struct hash<mir::PrefabId>
    {
        size_t operator()(mir::PrefabId id) const noexcept
        {
            const size_t high = hash<uint64_t>{}(id.mHigh);
            const size_t low = hash<uint64_t>{}(id.mLow);
            // golden-ratio combine
            return high ^ (low + 0x9e3779b97f4a7c15ULL + (high << 6) + (high >> 2));
        }
    };
}
