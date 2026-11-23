/**
 * @file AircraftFields.tests.cpp
 * @author Jackey Cen
 * @brief Tests for flight/components/Aircraft's config field and its serialized form.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/components/Aircraft.h>

#include <core/ecs/ComponentFields.h>

#include <scene/ComponentJson.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string>

namespace
{
    const mir::FieldDesc &ConfigField()
    {
        const auto found = std::ranges::find_if(mir::kAircraftFields,
            [](const mir::FieldDesc &field) { return field.mName == "config"; });
        REQUIRE(found != std::end(mir::kAircraftFields));
        return *found;
    }

    nlohmann::json AircraftJson(const mir::Aircraft &aircraft)
    {
        nlohmann::json document = nlohmann::json::object();
        for (const mir::FieldDesc &field : mir::kAircraftFields)
        {
            std::array<std::byte, 64> buffer{};
            field.Read(&aircraft, buffer.data());
            document[std::string(field.mName)] = mir::FieldToJson(field.mKind, buffer.data());
        }
        return document;
    }
} // namespace

TEST_CASE("The Aircraft config field picks *.aircraft.json files", "[flight][aircraft]")
{
    const mir::FieldDesc &field = ConfigField();
    CHECK(field.mKind == mir::FieldKind::String);
    CHECK(field.mWidget == mir::FieldWidget::AssetPath);
    CHECK(field.mAssetSuffix == ".aircraft.json");
}

TEST_CASE("An Aircraft writes the same fields as before", "[flight][aircraft]")
{
    REQUIRE(std::size(mir::kAircraftFields) == 3);
    REQUIRE(std::size(mir::kAircraftTransient) == 1);
    CHECK(mir::kAircraftTransient[0].mOffset == offsetof(mir::Aircraft, runtime));

    mir::Aircraft aircraft;
    std::strncpy(aircraft.config, "aircraft/old.aircraft.json", mir::kFieldStringCapacity - 1);
    aircraft.tier = mir::AerodynamicTier::L3;
    aircraft.playerInput = false;

    CHECK(AircraftJson(aircraft) ==
          nlohmann::json::parse(R"({"config":"aircraft/old.aircraft.json","tier":2,"playerInput":false})"));
}

TEST_CASE("An Aircraft's fields survive a Json round-trip", "[flight][aircraft]")
{
    mir::Aircraft aircraft;
    std::strncpy(aircraft.config, "aircraft/old.aircraft.json", mir::kFieldStringCapacity - 1);
    aircraft.tier = mir::AerodynamicTier::L3;
    aircraft.playerInput = false;
    const nlohmann::json document = AircraftJson(aircraft);

    mir::Aircraft restored;
    for (const mir::FieldDesc &field : mir::kAircraftFields)
    {
        std::array<std::byte, 64> buffer{};
        REQUIRE(mir::JsonToField(field.mKind, document.at(std::string(field.mName)), buffer.data()));
        REQUIRE(field.Write(&restored, buffer.data()));
    }

    CHECK(std::strcmp(restored.config, aircraft.config) == 0);
    CHECK(restored.tier == aircraft.tier);
    CHECK(restored.playerInput == aircraft.playerInput);
}
