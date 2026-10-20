/**
 * @file PrefabId.cpp
 * @author Jackey Cen
 * @brief Persistent 128-bit prefab identity (UUID v4), path-independent.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "prefab/PrefabId.h"

#include <array>
#include <random>

namespace mir
{
    namespace
    {
        constexpr std::string_view kHexDigits = "0123456789abcdef";

        uint8_t ByteAt(PrefabId id, int index)
        {
            if (index < 8)
                return static_cast<uint8_t>(id.mHigh >> (56 - 8 * index));
            return static_cast<uint8_t>(id.mLow >> (56 - 8 * (index - 8)));
        }

        int HexValue(char c)
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        }

        bool IsDashPosition(int index)
        {
            return index == 8 || index == 13 || index == 18 || index == 23;
        }
    }

    PrefabId GeneratePrefabId()
    {
        std::random_device rd;
        PrefabId id;
        id.mHigh = (static_cast<uint64_t>(rd()) << 32) | rd();
        id.mLow = (static_cast<uint64_t>(rd()) << 32) | rd();

        // RFC 4122 version 4 and variant bits
        id.mHigh = (id.mHigh & ~0xF000ULL) | 0x4000ULL;
        id.mLow = (id.mLow & ~(0xC0ULL << 56)) | (0x80ULL << 56);
        return id;
    }

    std::string ToString(PrefabId id)
    {
        std::string out(36, '0');
        int hexIndex = 0;
        for (int i = 0; i < 36; ++i)
        {
            if (IsDashPosition(i))
            {
                out[i] = '-';
                continue;
            }
            const uint8_t byte = ByteAt(id, hexIndex / 2);
            out[i] = kHexDigits[(hexIndex % 2 == 0) ? (byte >> 4) : (byte & 0xF)];
            ++hexIndex;
        }
        return out;
    }

    std::optional<PrefabId> ParsePrefabId(std::string_view text)
    {
        if (text.size() != 36)
            return std::nullopt;

        std::array<uint8_t, 16> bytes{};
        int hexIndex = 0;
        for (int i = 0; i < 36; ++i)
        {
            if (IsDashPosition(i))
            {
                if (text[static_cast<std::size_t>(i)] != '-')
                    return std::nullopt;
                continue;
            }
            const int digit = HexValue(text[static_cast<std::size_t>(i)]);
            if (digit < 0)
                return std::nullopt;
            uint8_t &byte = bytes[static_cast<std::size_t>(hexIndex / 2)];
            byte = static_cast<uint8_t>((hexIndex % 2 == 0) ? (digit << 4) : (byte | digit));
            ++hexIndex;
        }

        PrefabId id{};
        for (int i = 0; i < 8; ++i)
        {
            id.mHigh = (id.mHigh << 8) | bytes[static_cast<std::size_t>(i)];
            id.mLow = (id.mLow << 8) | bytes[static_cast<std::size_t>(i + 8)];
        }
        if (id.IsNull())
            return std::nullopt;
        return id;
    }
}
