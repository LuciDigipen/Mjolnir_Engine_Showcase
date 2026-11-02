/**
 * @file SurfaceMaterialLibrary.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/SurfaceMaterialLibrary.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/SurfaceMaterialTexture.h>
#include <assets/AssetId.h>
#include <renderer/SurfaceMaterialLibrary.h>
#include <renderer/ShaderReload.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

using namespace mir;

TEST_CASE("Surface texture fields use the documented names and aliases", "[renderer][material]")
{
    for (const char *name : {"albedo", "albedoTexture", "baseColor", "baseColorTexture"})
        CHECK(SurfaceTextureRoleForField(name) == SurfaceTextureRole::Albedo);
    for (const char *name : {"normal", "normalTexture"})
        CHECK(SurfaceTextureRoleForField(name) == SurfaceTextureRole::Normal);
    for (const char *name : {"surface", "surfaceTexture"})
        CHECK(SurfaceTextureRoleForField(name) == SurfaceTextureRole::Surface);
}

TEST_CASE("Surface texture field mapping rejects ambiguous names", "[renderer][material]")
{
    for (const char *name : {"baseColorNormal", "normalRoughness", "roughness", "mask", "albedoSurface", ""})
        CHECK_FALSE(SurfaceTextureRoleForField(name).has_value());
    for (const char *name : {"macro", "macroTexture"})
        CHECK(SurfaceTextureRoleForField(name) == SurfaceTextureRole::Macro);
}

TEST_CASE("Macro and parallax parameters come from the packed record", "[renderer][material]")
{
    MaterialLayout layout;
    const auto add = [&layout](const char *name, MaterialFieldKind kind, uint32_t offset, uint32_t size)
    {
        layout.fields.push_back(MaterialFieldLayout{.name = name, .kind = kind, .offset = offset, .size = size});
    };
    add("macroPeriodMeters", MaterialFieldKind::Float, 0, 4);
    add("macroStrength", MaterialFieldKind::Float, 4, 4);
    add("macroHueShiftDegrees", MaterialFieldKind::Float, 8, 4);
    add("macroRoughnessContrast", MaterialFieldKind::Float, 12, 4);
    add("parallaxEnabled", MaterialFieldKind::Bool, 16, 4);
    add("parallaxRtEnabled", MaterialFieldKind::Bool, 20, 4);
    add("parallaxHeightMeters", MaterialFieldKind::Float, 24, 4);
    add("parallaxReference", MaterialFieldKind::Float, 28, 4);
    add("parallaxDistanceMeters", MaterialFieldKind::Float, 32, 4);
    add("parallaxSteps", MaterialFieldKind::Uint, 36, 4);
    add("parallaxFadeStartMeters", MaterialFieldKind::Float, 40, 4);
    add("parallaxFadeEndMeters", MaterialFieldKind::Float, 44, 4);
    layout.size = 48;

    std::array<std::byte, 48> record{};
    const auto store = [&record](uint32_t offset, auto value)
    {
        std::memcpy(record.data() + offset, &value, sizeof(value));
    };
    store(0, 64.0f);
    store(4, 0.35f);
    store(8, 8.0f);
    store(12, 0.35f);
    store(16, 1u);
    store(20, 0u);
    store(24, 0.057f);
    store(28, 0.455f);
    store(32, 0.08f);
    store(36, 24u);
    store(40, 40.0f);
    store(44, 120.0f);

    const std::optional<SurfaceMaterialMacroParameters> macro =
        SurfaceMaterialMacroParametersFromRecord(layout, record);
    REQUIRE(macro);
    REQUIRE(macro->periodMeters);
    CHECK(*macro->periodMeters == 64.0);
    CHECK(macro->strength == Catch::Approx(0.35));
    CHECK(macro->hueShiftDegrees == Catch::Approx(8.0));
    CHECK(ValidateSurfaceMaterialMacroParameters(*macro, nullptr));

    const std::optional<SurfaceMaterialParallaxParameters> parallax =
        SurfaceMaterialParallaxParametersFromRecord(layout, record);
    REQUIRE(parallax);
    CHECK(parallax->enabled);
    CHECK_FALSE(parallax->rayTracedEnabled);
    CHECK(parallax->heightMeters == Catch::Approx(0.057));
    CHECK(parallax->reference == Catch::Approx(0.455));
    CHECK(parallax->steps == 24);
    CHECK(ValidateSurfaceMaterialParallaxParameters(*parallax, nullptr));
}

TEST_CASE("Material numeric bounds reject unsafe macro and parallax values", "[renderer][material]")
{
    SurfaceMaterialMacroParameters macro;
    macro.periodMeters = 0.0;
    CHECK_FALSE(ValidateSurfaceMaterialMacroParameters(macro, nullptr));
    macro.periodMeters = 64.0;
    macro.strength = 1.5;
    CHECK_FALSE(ValidateSurfaceMaterialMacroParameters(macro, nullptr));
    macro.strength = 0.35;
    macro.hueShiftDegrees = 181.0;
    CHECK_FALSE(ValidateSurfaceMaterialMacroParameters(macro, nullptr));

    SurfaceMaterialParallaxParameters parallax;
    parallax.heightMeters = 0.057;
    parallax.reference = 0.455;
    parallax.maxDistanceMeters = 0.08;
    parallax.steps = 24;
    parallax.fadeEndMeters = 120.0;
    CHECK(ValidateSurfaceMaterialParallaxParameters(parallax, nullptr));
    parallax.heightMeters = 0.6;
    CHECK_FALSE(ValidateSurfaceMaterialParallaxParameters(parallax, nullptr));
    parallax.heightMeters = 0.057;
    parallax.steps = 129;
    CHECK_FALSE(ValidateSurfaceMaterialParallaxParameters(parallax, nullptr));
    parallax.steps = 24;
    parallax.fadeStartMeters = 130.0;
    CHECK_FALSE(ValidateSurfaceMaterialParallaxParameters(parallax, nullptr));
}

TEST_CASE("The macro texture is deterministic and non-uniform", "[renderer][material]")
{
    std::vector<std::byte> first;
    std::vector<std::byte> second;
    BuildSurfaceMacroTexture(256, first);
    BuildSurfaceMacroTexture(256, second);
    REQUIRE(first.size() == 256u * 256u * 4u);
    CHECK(first == second);

    const std::byte third = std::byte{255};
    CHECK(first[3] == third);
    bool distinct = false;
    for (size_t index = 0; index < first.size() && !distinct; index += 4)
        distinct = first[index] != first[0];
    CHECK(distinct);
}

TEST_CASE("Terrain tile periods come from the packed float record", "[renderer][material]")
{
    MaterialLayout layout;
    layout.fields.push_back(MaterialFieldLayout{.name = "tileSizeMeters",
                                                .kind = MaterialFieldKind::Float,
                                                .offset = 0,
                                                .size = 4});
    layout.size = 16;
    std::array<std::byte, 16> record{};

    const float two = 2.0f;
    std::memcpy(record.data(), &two, sizeof(two));
    const std::optional<double> period = SurfaceMaterialTilePeriodFromRecord(layout, record);
    REQUIRE(period);
    CHECK(*period == 2.0);

    MaterialLayout absent;
    absent.size = 16;
    CHECK_FALSE(SurfaceMaterialTilePeriodFromRecord(absent, record).has_value());
}

TEST_CASE("A committed record adoption keeps the live handle, texture rows and reordered values",
    "[renderer][material]")
{
    MaterialLayout accepted;
    accepted.fields = {
        {.name = "tileSizeMeters", .kind = MaterialFieldKind::Float, .offset = 0, .size = 4},
        {.name = "albedo", .kind = MaterialFieldKind::TextureRef, .offset = 8, .size = 8},
        {.name = "macroPeriodMeters", .kind = MaterialFieldKind::Float, .offset = 16, .size = 4},
    };
    accepted.size = 32;
    const auto acceptedBytes = PackMaterialRecord(accepted,
        R"({"tileSizeMeters": 2, "albedo": {"image": 11, "sampler": 3}, "macroPeriodMeters": 64})");
    REQUIRE(acceptedBytes);

    MaterialLayout committed;
    committed.fields = {
        {.name = "macroPeriodMeters", .kind = MaterialFieldKind::Float, .offset = 0, .size = 4},
        {.name = "tileSizeMeters", .kind = MaterialFieldKind::Float, .offset = 4, .size = 4},
        {.name = "albedo", .kind = MaterialFieldKind::TextureRef, .offset = 8, .size = 8},
        {.name = "macroStrength", .kind = MaterialFieldKind::Float, .offset = 16, .size = 4},
    };
    committed.size = 32;
    const auto defaults = CompleteMaterialParameters(committed, "{}", R"({"macroStrength": 0.35})");
    REQUIRE(defaults);
    const auto defaultBytes = PackMaterialRecord(committed, *defaults);
    REQUIRE(defaultBytes);

    const MaterialMigration migrated = MigrateMaterialRecord(accepted, committed, *acceptedBytes, *defaultBytes);
    INFO(migrated.error);
    REQUIRE(migrated.ok);

    SurfaceMaterialBinding binding;
    binding.material = MaterialHandle{ 7, 2 };
    std::string error;
    const std::optional<SurfaceMaterialSchemaAdoption> adopted =
        AdoptSurfaceMaterialRecord(binding, committed, migrated.bytes, &error);
    INFO(error);
    REQUIRE(adopted);
    CHECK(adopted->binding.material == MaterialHandle{ 7, 2 });
    CHECK(adopted->revision == MaterialLayoutRevision(committed));
    REQUIRE(adopted->binding.tilePeriodMeters);
    CHECK(*adopted->binding.tilePeriodMeters == 2.0);
    REQUIRE(adopted->binding.macro);
    CHECK(adopted->binding.macro->periodMeters == 64.0);
    CHECK(adopted->binding.macro->strength == Catch::Approx(0.35));

    const nlohmann::json parameters = nlohmann::json::parse(adopted->parametersJson);
    CHECK(parameters.at("tileSizeMeters").get<double>() == Catch::Approx(2.0));
    CHECK(parameters.at("macroStrength").get<double>() == Catch::Approx(0.35));
    CHECK(parameters.at("albedo").at("image") == 11);
    CHECK(parameters.at("albedo").at("sampler") == 3);
    CHECK_FALSE(MaterialRecordParameters(committed, std::span<const std::byte>(migrated.bytes).first(16)).has_value());

    MaterialLayout retyped = committed;
    retyped.fields[1].kind = MaterialFieldKind::Uint;
    const MaterialMigration rejected = MigrateMaterialRecord(accepted, retyped, *acceptedBytes, *defaultBytes);
    CHECK_FALSE(rejected.ok);
    CHECK(rejected.error.find("changed type") != std::string::npos);
}

TEST_CASE("Terrain and mesh owners merge their active material uses", "[renderer][material]")
{
    SurfaceMaterialLibrary library;
    CHECK(library.ActiveMaterials().empty());

    const AssetId terrainAsset{ 1 };
    const AssetId meshAsset{ 2 };
    library.SetActiveMaterials(std::vector<SurfaceMaterialUse>{
        { .logicalPath = "assets/materials/shared.material.json", .asset = terrainAsset, .frontend = "terrain" } });

    const uint64_t mainOwner = library.AddMaterialUseOwner();
    const uint64_t previewOwner = library.AddMaterialUseOwner();
    library.SetMaterialUses(mainOwner, std::vector<SurfaceMaterialUse>{
        { .logicalPath = "assets/materials/shared.material.json", .asset = meshAsset, .frontend = "mesh" } });
    library.SetMaterialUses(previewOwner, std::vector<SurfaceMaterialUse>{
        { .logicalPath = "assets/materials/shared.material.json", .asset = meshAsset, .frontend = "mesh",
          .required = true } });

    REQUIRE(library.ActiveMaterials().size() == 2);
    bool meshRequired = false;
    bool terrainPresent = false;
    for (const SurfaceMaterialUse &use : library.ActiveMaterials())
    {
        if (use.frontend == "mesh")
            meshRequired = use.required;
        if (use.frontend == "terrain")
            terrainPresent = true;
    }
    CHECK(meshRequired);
    CHECK(terrainPresent);

    library.RemoveMaterialUseOwner(previewOwner);
    REQUIRE(library.ActiveMaterials().size() == 2);
    library.RemoveMaterialUseOwner(mainOwner);
    REQUIRE(library.ActiveMaterials().size() == 1);
    CHECK(library.ActiveMaterials()[0].frontend == "terrain");
}

TEST_CASE("Shared bounds apply to every frontend and the terrain phase rule only to terrain",
    "[renderer][material]")
{
    SurfaceMaterialLibrary library;
    bool terrainCalled = false;
    library.SetMaterialValidator([&](const SurfaceMaterialBinding &, std::string &error)
        {
            terrainCalled = true;
            error = "unsupported period";
            return false;
        });

    MaterialLayout layout;
    layout.size = 16;
    layout.fields.push_back({.name = "tileSizeMeters", .kind = MaterialFieldKind::Float,
        .offset = 0, .size = 4});
    layout.fields.push_back({.name = "parallaxEnabled", .kind = MaterialFieldKind::Bool,
        .offset = 4, .size = 4});
    layout.fields.push_back({.name = "parallaxHeightMeters", .kind = MaterialFieldKind::Float,
        .offset = 8, .size = 4});
    const auto meshPeriod = PackMaterialRecord(layout,
        R"({"tileSizeMeters": 7, "parallaxEnabled": false, "parallaxHeightMeters": 0.02})");
    const auto badParallax = PackMaterialRecord(layout,
        R"({"tileSizeMeters": 7, "parallaxEnabled": false, "parallaxHeightMeters": 0.6})");
    REQUIRE(meshPeriod);
    REQUIRE(badParallax);

    std::string error;
    CHECK(library.ValidateRecord("mesh", layout, *meshPeriod, error));
    CHECK_FALSE(terrainCalled);
    CHECK_FALSE(library.ValidateRecord("mesh", layout, *badParallax, error));
    CHECK(error.find("parallaxHeightMeters") != std::string::npos);

    CHECK_FALSE(library.ValidateRecord("terrain", layout, *meshPeriod, error));
    CHECK(terrainCalled);

    MaterialLayout badPeriod;
    badPeriod.size = 16;
    badPeriod.fields.push_back({.name = "tileSizeMeters", .kind = MaterialFieldKind::Float,
        .offset = 0, .size = 4});
    const auto zeroPeriod = PackMaterialRecord(badPeriod, R"({"tileSizeMeters": 0})");
    REQUIRE(zeroPeriod);
    CHECK_FALSE(library.ValidateRecord("mesh", badPeriod, *zeroPeriod, error));
    CHECK(error.find("tileSizeMeters") != std::string::npos);
}

TEST_CASE("A mesh record repairs a terrain frontend once its values fit the phase rule",
    "[renderer][material]")
{
    SurfaceMaterialLibrary library;
    library.SetMaterialValidator([](const SurfaceMaterialBinding &binding, std::string &error)
        {
            if (binding.tilePeriodMeters && *binding.tilePeriodMeters != 2.0)
            {
                error = "unsupported period";
                return false;
            }
            return true;
        });

    MaterialLayout mesh;
    mesh.size = 16;
    mesh.fields.push_back({.name = "tileSizeMeters", .kind = MaterialFieldKind::Float,
        .offset = 0, .size = 4});
    MaterialLayout terrain = mesh;
    terrain.fields.push_back({.name = "parallaxHeightMeters", .kind = MaterialFieldKind::Float,
        .offset = 4, .size = 4});
    terrain.size = 16;

    const auto meshRecord = PackMaterialRecord(mesh, R"({"tileSizeMeters": 7})");
    REQUIRE(meshRecord);

    // The later frontend merges the live peer tile period, which the terrain
    // consumer rejects; no baseline binding may be published from the cooked 2.
    std::string error;
    const auto inherited = MergePeerNumericParameters(terrain,
        R"({"tileSizeMeters": 2, "parallaxHeightMeters": 0.02})", mesh, *meshRecord, &error);
    INFO(error);
    REQUIRE(inherited);
    const auto terrainRecord = PackMaterialRecord(terrain, *inherited);
    REQUIRE(terrainRecord);
    CHECK_FALSE(library.ValidateRecord("terrain", terrain, *terrainRecord, error));

    const auto repairedMesh = PackMaterialRecord(mesh, R"({"tileSizeMeters": 2})");
    REQUIRE(repairedMesh);
    const auto repaired = MergePeerNumericParameters(terrain,
        R"({"tileSizeMeters": 2, "parallaxHeightMeters": 0.02})", mesh, *repairedMesh, &error);
    REQUIRE(repaired);
    const auto repairedRecord = PackMaterialRecord(terrain, *repaired);
    REQUIRE(repairedRecord);
    CHECK(library.ValidateRecord("terrain", terrain, *repairedRecord, error));
}

TEST_CASE("A caller record callback runs once per proposed record and can reject it",
    "[renderer][material]")
{
    SurfaceMaterialLibrary library;
    int callbacks = 0;
    bool reject = false;
    library.SetRecordValidator([&](const MaterialLayout &, std::span<const std::byte>, std::string &error)
        {
            ++callbacks;
            if (!reject)
                return true;
            error = "second frontend rejected";
            return false;
        });

    MaterialLayout layout;
    layout.size = 16;
    layout.fields.push_back({.name = "tint", .kind = MaterialFieldKind::Float3, .offset = 0, .size = 12});
    const auto record = PackMaterialRecord(layout, R"({"tint": [1, 0, 0]})");
    REQUIRE(record);

    std::string error;
    CHECK(library.ValidateRecordForPublication("mesh", layout, *record, error));
    CHECK(callbacks == 1);

    reject = true;
    CHECK_FALSE(library.ValidateRecordForPublication("terrain", layout, *record, error));
    CHECK(callbacks == 2);
    CHECK(error == "second frontend rejected");

    // The generic bounds still run first: a bad value never reaches the callback.
    const auto bad = PackMaterialRecord(layout, R"({"tint": [1, 0, 0]})");
    REQUIRE(bad);
    MaterialLayout parallax = layout;
    parallax.fields.push_back({.name = "parallaxHeightMeters", .kind = MaterialFieldKind::Float,
        .offset = 12, .size = 4});
    parallax.size = 16;
    const auto badParallax = PackMaterialRecord(parallax, R"({"tint": [1, 0, 0], "parallaxHeightMeters": 0.6})");
    REQUIRE(badParallax);
    callbacks = 0;
    CHECK_FALSE(library.ValidateRecordForPublication("mesh", parallax, *badParallax, error));
    CHECK(callbacks == 0);
}

TEST_CASE("Replacing an owner's uses drops the assets no live consumer names", "[renderer][material]")
{
    SurfaceMaterialLibrary library;
    library.SetActiveMaterials(std::vector<SurfaceMaterialUse>{
        { .logicalPath = "assets/materials/terrain.material.json", .asset = AssetId{ 3 }, .frontend = "terrain" } });
    const uint64_t owner = library.AddMaterialUseOwner();
    library.SetMaterialUses(owner, std::vector<SurfaceMaterialUse>{
        { .logicalPath = "assets/materials/a.material.json", .asset = AssetId{ 1 }, .frontend = "mesh",
          .required = true },
        { .logicalPath = "assets/materials/b.material.json", .asset = AssetId{ 2 }, .frontend = "mesh",
          .required = true } });
    REQUIRE(library.ActiveMaterials().size() == 3);

    // A mesh entity switched to another asset: its former asset is no longer required.
    library.SetMaterialUses(owner, std::vector<SurfaceMaterialUse>{
        { .logicalPath = "assets/materials/b.material.json", .asset = AssetId{ 2 }, .frontend = "mesh",
          .required = true } });
    REQUIRE(library.ActiveMaterials().size() == 2);
    for (const SurfaceMaterialUse &use : library.ActiveMaterials())
        CHECK(use.logicalPath != "assets/materials/a.material.json");

    library.RemoveMaterialUseOwner(owner);
    REQUIRE(library.ActiveMaterials().size() == 1);
    CHECK(library.ActiveMaterials()[0].frontend == "terrain");
}

TEST_CASE("Migrated terrain records pass through the consumer validator", "[renderer][material]")
{
    SurfaceMaterialLibrary library;
    library.SetMaterialValidator([](const SurfaceMaterialBinding &binding, std::string &error)
        {
            if (binding.tilePeriodMeters && *binding.tilePeriodMeters != 2.0)
            {
                error = "unsupported period";
                return false;
            }
            return true;
        });
    MaterialLayout previous;
    previous.size = 16;
    MaterialLayout candidate;
    candidate.size = 16;
    candidate.fields.push_back({.name = "tileSizeMeters", .kind = MaterialFieldKind::Float,
        .offset = 0, .size = 4});
    const auto original = PackMaterialRecord(previous, "{}");
    const auto invalid = PackMaterialRecord(candidate, R"({"tileSizeMeters":7})");
    const auto valid = PackMaterialRecord(candidate, R"({"tileSizeMeters":2})");
    REQUIRE(original);
    REQUIRE(invalid);
    REQUIRE(valid);
    const auto rejected = MigrateMaterialRecord(previous, candidate, *original, *invalid);
    const auto accepted = MigrateMaterialRecord(previous, candidate, *original, *valid);
    REQUIRE(rejected.ok);
    REQUIRE(accepted.ok);
    std::string error;
    CHECK_FALSE(library.ValidateRecord("terrain", candidate, rejected.bytes, error));
    CHECK(error == "unsupported period");
    CHECK(library.ValidateRecord("terrain", candidate, accepted.bytes, error));
    CHECK(library.ValidateRecord("static", candidate, rejected.bytes, error));
}

TEST_CASE("Terrain record validation includes macro and parallax parameters", "[renderer][material]")
{
    SurfaceMaterialLibrary library;
    library.SetMaterialValidator([](const SurfaceMaterialBinding &binding, std::string &error)
        {
            if (!binding.macro || !binding.parallax)
            {
                error = "missing shading parameters";
                return false;
            }
            return ValidateSurfaceMaterialMacroParameters(*binding.macro, &error) &&
                ValidateSurfaceMaterialParallaxParameters(*binding.parallax, &error);
        });
    MaterialLayout layout;
    layout.size = 16;
    layout.fields.push_back({.name = "macroPeriodMeters", .kind = MaterialFieldKind::Float,
        .offset = 0, .size = 4});
    layout.fields.push_back({.name = "parallaxHeightMeters", .kind = MaterialFieldKind::Float,
        .offset = 4, .size = 4});
    layout.fields.push_back({.name = "parallaxFadeEndMeters", .kind = MaterialFieldKind::Float,
        .offset = 8, .size = 4});
    const auto valid = PackMaterialRecord(layout,
        R"({"macroPeriodMeters":64,"parallaxHeightMeters":0.057,"parallaxFadeEndMeters":120})");
    const auto invalidMacro = PackMaterialRecord(layout,
        R"({"macroPeriodMeters":0,"parallaxHeightMeters":0.057,"parallaxFadeEndMeters":120})");
    const auto invalidParallax = PackMaterialRecord(layout,
        R"({"macroPeriodMeters":64,"parallaxHeightMeters":0.6,"parallaxFadeEndMeters":120})");
    REQUIRE(valid);
    REQUIRE(invalidMacro);
    REQUIRE(invalidParallax);
    std::string error;
    CHECK(library.ValidateRecord("terrain", layout, *valid, error));
    CHECK_FALSE(library.ValidateRecord("terrain", layout, *invalidMacro, error));
    CHECK_FALSE(library.ValidateRecord("terrain", layout, *invalidParallax, error));
}

TEST_CASE("Shared material overlays retain unknown-key and texture-edit rejection", "[renderer][material]")
{
    MaterialLayout mesh;
    mesh.size = 16;
    mesh.fields = {
        {.name = "roughness", .kind = MaterialFieldKind::Float, .offset = 0, .size = 4},
        {.name = "albedo", .kind = MaterialFieldKind::TextureRef, .offset = 8, .size = 8},
    };
    MaterialLayout terrain;
    terrain.size = 16;
    terrain.fields = {
        {.name = "tileSizeMeters", .kind = MaterialFieldKind::Float, .offset = 0, .size = 4},
    };
    const std::array<MaterialLayout, 2> layouts{mesh, terrain};
    std::string error;
    CHECK(ValidateSurfaceMaterialParameterOverlay(layouts,
        R"({"roughness":0.5,"tileSizeMeters":2})", &error));

    CHECK_FALSE(ValidateSurfaceMaterialParameterOverlay(layouts, R"({"roughnes":0.5})", &error));
    CHECK(error.find("roughnes") != std::string::npos);
    CHECK(error.find("not in the reflected layout") != std::string::npos);

    CHECK_FALSE(ValidateSurfaceMaterialParameterOverlay(layouts,
        R"({"albedo":{"image":7,"sampler":2}})", &error));
    CHECK(error.find("source recipe") != std::string::npos);
    CHECK_FALSE(ValidateSurfaceMaterialParameterOverlay(layouts, "[]", &error));
    CHECK_FALSE(ValidateSurfaceMaterialParameterOverlay(layouts, "{", &error));
}
