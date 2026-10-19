/**
 * @file PrefabId.tests.cpp
 * @author Jackey Cen
 * @brief Tests for prefab/PrefabId.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <prefab/PrefabId.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <unordered_set>

namespace
{
    using mir::PrefabId;
}

TEST_CASE("Generated PrefabIds round-trip through canonical text", "[prefab]")
{
    const PrefabId first = mir::GeneratePrefabId();
    const PrefabId second = mir::GeneratePrefabId();

    REQUIRE_FALSE(first.IsNull());
    REQUIRE(first != second);

    const std::string text = mir::ToString(first);
    REQUIRE(text.size() == 36);
    REQUIRE(text[8] == '-');
    REQUIRE(text[13] == '-');
    REQUIRE(text[18] == '-');
    REQUIRE(text[23] == '-');
    REQUIRE(text[14] == '4');
    REQUIRE((text[19] == '8' || text[19] == '9' || text[19] == 'a' || text[19] == 'b'));
    REQUIRE(std::all_of(text.begin(), text.end(), [](char c)
        {
            return c == '-' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }));

    const std::optional<PrefabId> parsed = mir::ParsePrefabId(text);
    REQUIRE(parsed.has_value());
    REQUIRE(*parsed == first);
}

TEST_CASE("ParsePrefabId accepts uppercase hex", "[prefab]")
{
    const PrefabId id = mir::GeneratePrefabId();
    std::string upper = mir::ToString(id);
    std::transform(upper.begin(), upper.end(), upper.begin(), [](char c)
        {
            return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        });

    const std::optional<PrefabId> parsed = mir::ParsePrefabId(upper);
    REQUIRE(parsed.has_value());
    REQUIRE(*parsed == id);
}

TEST_CASE("ParsePrefabId rejects malformed text and the nil UUID", "[prefab]")
{
    REQUIRE(mir::ParsePrefabId("00000000-0000-0000-0000-000000000000") == std::nullopt);
    REQUIRE(mir::ParsePrefabId("") == std::nullopt);
    REQUIRE(mir::ParsePrefabId("12345678123442348234123456789012") == std::nullopt);
    REQUIRE(mir::ParsePrefabId("123456781-234-4234-8234-123456789012") == std::nullopt);
    REQUIRE(mir::ParsePrefabId("12345678-1234-4234-8234-12345678901") == std::nullopt);
    REQUIRE(mir::ParsePrefabId("12345678-1234-4234-8234-1234567890123") == std::nullopt);
    REQUIRE(mir::ParsePrefabId("12345678-1234-4234-8234-12345678901g") == std::nullopt);
    REQUIRE(mir::ParsePrefabId("12345678 1234-4234-8234-123456789012") == std::nullopt);
    REQUIRE(mir::ParsePrefabId("12345678-1234-4234-8234-123456789012") != std::nullopt);
}

TEST_CASE("PrefabId orders and hashes", "[prefab]")
{
    REQUIRE(mir::kNullPrefabId.IsNull());

    const std::optional<PrefabId> low = mir::ParsePrefabId("00000000-0000-4000-8000-000000000001");
    const std::optional<PrefabId> high = mir::ParsePrefabId("00000000-0000-4000-8000-000000000002");
    REQUIRE(low.has_value());
    REQUIRE(high.has_value());
    REQUIRE(*low < *high);
    REQUIRE(*high > *low);

    std::unordered_set<PrefabId> ids;
    ids.insert(*low);
    ids.insert(*high);
    ids.insert(mir::GeneratePrefabId());
    REQUIRE(ids.size() == 3);
    REQUIRE(ids.contains(*low));
}
