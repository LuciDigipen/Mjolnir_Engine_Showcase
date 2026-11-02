/**
 * @file MaterialDraft.tests.cpp
 * @author Jackey Cen
 * @brief Tests for editor/MaterialDraft.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/MaterialDraft.h>

#include <core/fs/FileIO.h>
#include <serial/JsonFile.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    std::filesystem::path MakeScratchFile()
    {
        static std::atomic<int> counter{ 0 };
        std::random_device rd;
        std::ostringstream token;
        token << std::hex << ((static_cast<std::uint64_t>(rd()) << 32) ^ rd());
        return std::filesystem::temp_directory_path() /
            ("mjolnir_material_draft_test_" + token.str() + "_" + std::to_string(counter.fetch_add(1)) + ".json");
    }

    mir::MaterialLayout TestLayout()
    {
        mir::MaterialLayout layout;
        layout.fields = {
            {.name = "amount", .kind = mir::MaterialFieldKind::Float, .offset = 0, .size = 4},
            {.name = "count", .kind = mir::MaterialFieldKind::Int, .offset = 4, .size = 4},
            {.name = "index", .kind = mir::MaterialFieldKind::Uint, .offset = 8, .size = 4},
            {.name = "flag", .kind = mir::MaterialFieldKind::Bool, .offset = 12, .size = 4},
            {.name = "color", .kind = mir::MaterialFieldKind::Float3, .offset = 16, .size = 12},
            {.name = "tint", .kind = mir::MaterialFieldKind::Float4, .offset = 32, .size = 16},
            {.name = "uv", .kind = mir::MaterialFieldKind::Float2, .offset = 48, .size = 8},
            {.name = "albedo", .kind = mir::MaterialFieldKind::TextureRef, .offset = 56, .size = 8},
            {.name = "macroTextureRef", .kind = mir::MaterialFieldKind::TextureRef, .offset = 64, .size = 4},
        };
        layout.size = 80;
        return layout;
    }

    mir::MaterialRecipe TestRecipe()
    {
        mir::MaterialRecipe recipe;
        recipe.shaderFile = "materials/test.slang";
        recipe.shaderType = "TestMaterial";
        recipe.frontends = {"terrain"};
        recipe.parametersJson = R"({
            "amount": 1.0, "count": 2, "index": 3, "flag": true,
            "color": [0.1, 0.2, 0.3], "tint": [1.0, 1.0, 1.0, 1.0], "uv": [1.0, 2.0],
            "unknownParameter": 7.0
        })";
        recipe.textures = {
            {.role = "baseColor", .path = "textures/base.png", .colorSpace = "srgb"},
            {.role = "ao", .path = "textures/ao.png", .colorSpace = "srgb"},
            {.role = "normal", .path = "textures/normal.png", .colorSpace = "linear"},
            {.role = "roughness", .path = "textures/rough.png", .colorSpace = "linear"},
        };
        return recipe;
    }

    mir::MaterialDraft TestDraft()
    {
        std::string error;
        std::optional<mir::MaterialDraft> draft = mir::BuildMaterialDraft("games/Test|games/Test/assets/materials/test.material.json",
            "games/Test/assets/materials/test.material.json", "games/Test", "terrain", TestRecipe(),
            TestLayout(), &error);
        REQUIRE(draft);
        return *draft;
    }

    mir::MaterialDraftField *Field(mir::MaterialDraft &draft, std::string_view name)
    {
        for (mir::MaterialDraftField &field : draft.mFields)
            if (field.mLayout.name == name)
                return &field;
        return nullptr;
    }
}

TEST_CASE("A reflected draft converts scalar, vector, bool and texture fields", "[editor][material]")
{
    const mir::MaterialDraft draft = TestDraft();
    CHECK(draft.mFields.size() == 7);
    REQUIRE(draft.mTextureControls.size() == 2);
    CHECK(draft.mTextureControls[0].mField == "albedo");
    CHECK(draft.mTextureControls[0].mEditable);
    CHECK(draft.mTextureControls[0].mRecipeRoles == std::vector<std::string>{"baseColor", "ao"});
    CHECK(draft.mTextureControls[1].mField == "macroTextureRef");
    CHECK_FALSE(draft.mTextureControls[1].mEditable);

    CHECK(std::find(draft.mUnsupported.begin(), draft.mUnsupported.end(),
              std::string("parameter 'unknownParameter' is not reflected; the recipe value is preserved")) !=
        draft.mUnsupported.end());
    CHECK(std::find(draft.mUnsupported.begin(), draft.mUnsupported.end(),
              std::string("texture 'roughness' is not reflected; the recipe value is preserved")) !=
        draft.mUnsupported.end());

    const nlohmann::json parameters = nlohmann::json::parse(mir::MaterialDraftParametersJson(draft));
    CHECK(parameters.at("amount") == 1.0f);
    CHECK(parameters.at("count") == 2);
    CHECK(parameters.at("index") == 3u);
    CHECK(parameters.at("flag") == true);
    CHECK(parameters.at("color") == nlohmann::json::array({0.1f, 0.2f, 0.3f}));
    CHECK(parameters.at("uv") == nlohmann::json::array({1.0f, 2.0f}));

    const mir::MaterialFieldLayout integer{.name = "count", .kind = mir::MaterialFieldKind::Int, .offset = 0, .size = 4};
    CHECK_FALSE(mir::MaterialDraftFieldFromJson(integer, R"("two")").has_value());
    const mir::MaterialFieldLayout boolean{.name = "flag", .kind = mir::MaterialFieldKind::Bool, .offset = 0, .size = 4};
    CHECK_FALSE(mir::MaterialDraftFieldFromJson(boolean, "1").has_value());
    const mir::MaterialFieldLayout color{.name = "color", .kind = mir::MaterialFieldKind::Float3, .offset = 0, .size = 12};
    CHECK_FALSE(mir::MaterialDraftFieldFromJson(color, "[1.0, 2.0]").has_value());
    CHECK_FALSE(mir::MaterialDraftFieldFromJson(color, "[1.0, 2.0, 1e300]").has_value());
}

TEST_CASE("A material draft keeps unsaved values when the selection changes", "[editor][material]")
{
    mir::MaterialDraftStore store;
    mir::MaterialDraft first = TestDraft();
    Field(first, "amount")->mComponents[0] = 4.0f;
    const std::string firstIdentity = first.mIdentity;
    store.Put(std::move(first));

    mir::MaterialDraft second = TestDraft();
    second.mIdentity = "games/Test|games/Test/assets/materials/other.material.json";
    second.mLogicalPath = "games/Test/assets/materials/other.material.json";
    const std::string secondIdentity = second.mIdentity;
    store.Put(std::move(second));

    CHECK(store.Size() == 2);
    CHECK(store.HasDirty());
    CHECK(store.Find(firstIdentity) != nullptr);
    CHECK(store.Find(firstIdentity)->mFields[0].mComponents[0] == 4.0f);

    CHECK(store.Find(secondIdentity) != nullptr);
    CHECK_FALSE(mir::MaterialDraftDirty(*store.Find(secondIdentity)));
    CHECK(store.Find(firstIdentity) != nullptr);
}

TEST_CASE("A material draft identity normalizes path spelling and project", "[editor][material]")
{
    const std::optional<std::string> first =
        mir::MaterialDraftIdentity("games/Test", "games\\Test\\assets\\materials\\..\\materials\\a.material.json");
    const std::optional<std::string> second =
        mir::MaterialDraftIdentity("./games/Test/", "games/Test/assets/materials/a.material.json");
    REQUIRE(first);
    REQUIRE(second);
    CHECK(*first == *second);
    CHECK(*first != *mir::MaterialDraftIdentity("games/Other", "games/Test/assets/materials/a.material.json"));
    CHECK_FALSE(mir::MaterialDraftIdentity("games/Test", "../escape/a.material.json").has_value());
}

TEST_CASE("A material save then cook moves the baseline; failure keeps the draft recoverable", "[editor][material]")
{
    mir::MaterialDraft draft = TestDraft();
    Field(draft, "amount")->mComponents[0] = 5.0f;
    CHECK(mir::MaterialDraftDirty(draft));
    const nlohmann::json patch = nlohmann::json::parse(mir::MaterialDraftDirtyParametersJson(draft));
    CHECK(patch.size() == 1);
    CHECK(patch.at("amount") == 5.0f);

    mir::MaterialDraftMarkSaved(draft, 1, 10, true);
    CHECK(draft.mCooking);
    CHECK(mir::MaterialDraftDirty(draft));

    Field(draft, "color")->mComponents[0] = 0.9f;
    CHECK(mir::MaterialDraftCooked(draft, true, {}));
    CHECK_FALSE(draft.mCooking);
    CHECK(mir::MaterialDraftDirty(draft));
    CHECK(draft.mBaselineFields[0].mComponents == Field(draft, "amount")->mComponents);
    CHECK(draft.mBaselineFields[4].mComponents[0] == 0.1f);

    mir::MaterialDraftRevert(draft);
    CHECK_FALSE(mir::MaterialDraftDirty(draft));
    CHECK(Field(draft, "color")->mComponents[0] == 0.1f);

    Field(draft, "amount")->mComponents[0] = 6.0f;
    mir::MaterialDraftMarkSaved(draft, 1, 10, true);
    CHECK_FALSE(mir::MaterialDraftCooked(draft, false, "the cook failed"));
    CHECK_FALSE(draft.mCooking);
    CHECK(mir::MaterialDraftDirty(draft));
    CHECK(Field(draft, "amount")->mComponents[0] == 6.0f);
    CHECK(draft.mStatusIsError);
    CHECK(draft.mStatus.find("cook failed") != std::string::npos);
}

TEST_CASE("A material preview is suppressed in Play and after the material leaves the scene", "[editor][material]")
{
    mir::MaterialDraft draft = TestDraft();
    Field(draft, "amount")->mComponents[0] = 5.0f;

    CHECK_FALSE(mir::MaterialDraftPreviewParameters(draft, false, true, 1).has_value());
    const std::optional<std::string> parameters = mir::MaterialDraftPreviewParameters(draft, true, true, 1);
    REQUIRE(parameters);
    CHECK(nlohmann::json::parse(*parameters).at("amount") == 5.0f);

    mir::MaterialDraftPreviewed(draft, 1, *parameters);
    CHECK_FALSE(mir::MaterialDraftPreviewParameters(draft, true, true, 1).has_value());
    CHECK(mir::MaterialDraftPreviewParameters(draft, true, true, 2).has_value());

    CHECK_FALSE(mir::MaterialDraftPreviewParameters(draft, true, false, 2).has_value());
    CHECK(mir::MaterialDraftPreviewParameters(draft, true, true, 2).has_value());
}

TEST_CASE("A texture draft changes only the authored recipe path", "[editor][material]")
{
    mir::MaterialDraft draft = TestDraft();
    const auto source = std::find_if(draft.mTextures.begin(), draft.mTextures.end(),
        [](const mir::MaterialTextureSource &texture) { return texture.mRole == "baseColor"; });
    REQUIRE(source != draft.mTextures.end());
    source->mPath = "textures/replacement.png";
    CHECK(mir::MaterialDraftDirty(draft));
    CHECK(mir::MaterialDraftDirtyTexturePaths(draft) ==
        std::map<std::string, std::string>{{"baseColor", "textures/replacement.png"}});
    CHECK(source->mColorSpace == "srgb");

    mir::MaterialDraftRevert(draft);
    CHECK_FALSE(mir::MaterialDraftDirty(draft));
    CHECK(source->mPath == "textures/base.png");
    CHECK(mir::MaterialDraftBaselineTexturePaths(draft).empty());
}

TEST_CASE("Material color controls come from field names, not a material table", "[editor][material]")
{
    CHECK(mir::MaterialFieldIsColor("albedoTint"));
    CHECK(mir::MaterialFieldIsColor("emissiveColor"));
    CHECK(mir::MaterialFieldIsColor("base_color"));
    CHECK_FALSE(mir::MaterialFieldIsColor("albedoRoughness"));
    CHECK_FALSE(mir::MaterialFieldIsColor("tileSizeMeters"));
    CHECK(mir::MaterialTextureRecipeRoles("albedo") == std::vector<std::string>{"baseColor", "ao"});
    CHECK(mir::MaterialTextureRecipeRoles("surface") ==
        std::vector<std::string>{"roughness", "displacement", "specular"});
    CHECK(mir::MaterialTextureRecipeRoles("macro").empty());
}

TEST_CASE("A same-size schema reorder migrates by field name and preserves unsaved values", "[editor][material]")
{
    mir::MaterialDraft draft = TestDraft();
    Field(draft, "amount")->mComponents[0] = 5.0f;

    mir::MaterialLayout reordered = TestLayout();
    reordered.fields[0].offset = 4;
    reordered.fields[1].offset = 0;

    std::string error;
    const std::optional<mir::MaterialDraftRefresh> refresh =
        mir::RefreshMaterialDraft(draft, TestRecipe(), reordered, draft.mFrontend, "reordered", &error);
    INFO(error);
    REQUIRE(refresh);

    const mir::MaterialDraftField *amount = nullptr;
    const mir::MaterialDraftField *count = nullptr;
    for (const mir::MaterialDraftField &field : draft.mFields)
    {
        if (field.mLayout.name == "amount")
            amount = &field;
        if (field.mLayout.name == "count")
            count = &field;
    }
    REQUIRE(amount != nullptr);
    REQUIRE(count != nullptr);
    CHECK(amount->mLayout.offset == 4);
    CHECK(amount->mComponents[0] == 5.0f);
    CHECK(count->mLayout.offset == 0);
    CHECK(count->mSigned == 2);
    CHECK(mir::MaterialDraftDirty(draft));
    CHECK(draft.mSchemaRevision == "reordered");

    const std::optional<std::string> overlay = mir::MaterialDraftPreviewParameters(draft, true, true, 1);
    REQUIRE(overlay);
    CHECK(nlohmann::json::parse(*overlay).at("amount") == 5.0f);
    CHECK_FALSE(nlohmann::json::parse(*overlay).contains("count"));
}

TEST_CASE("A frontend switch refreshes its target and layout together", "[editor][material]")
{
    mir::MaterialDraft draft = TestDraft();
    mir::MaterialRecipe recipe = TestRecipe();
    recipe.frontends = {"terrain", "mesh"};
    mir::MaterialLayout meshLayout = TestLayout();
    meshLayout.fields[0].offset = 4;
    meshLayout.fields[1].offset = 0;
    mir::MaterialDraftPreviewed(draft, 10, R"({"amount":1.0})");
    REQUIRE(draft.mPreviewedActive);

    std::string error;
    REQUIRE(mir::RefreshMaterialDraft(draft, recipe, meshLayout, "mesh", "mesh-schema", &error));
    CHECK(draft.mFrontend == "mesh");
    CHECK(Field(draft, "amount")->mLayout.offset == 4);
    CHECK(Field(draft, "count")->mLayout.offset == 0);
    CHECK(draft.mSchemaRevision == "mesh-schema");
    CHECK_FALSE(draft.mPreviewedActive);

    Field(draft, "amount")->mComponents[0] = 5.0f;
    const std::optional<std::string> overlay = mir::MaterialDraftPreviewParameters(draft, true, true, 11);
    REQUIRE(overlay);
    CHECK(nlohmann::json::parse(*overlay).at("amount") == 5.0f);

    REQUIRE(mir::RefreshMaterialDraft(draft, recipe, TestLayout(), "terrain", "terrain-schema", &error));
    CHECK(draft.mFrontend == "terrain");
    CHECK(Field(draft, "amount")->mLayout.offset == 0);
    CHECK(Field(draft, "amount")->mComponents[0] == 5.0f);
}

TEST_CASE("A rejected frontend refresh preserves the previous target and draft", "[editor][material]")
{
    mir::MaterialDraft draft = TestDraft();
    Field(draft, "amount")->mComponents[0] = 5.0f;
    mir::MaterialRecipe malformed = TestRecipe();
    malformed.parametersJson = "[]";
    std::string error;
    CHECK_FALSE(mir::RefreshMaterialDraft(draft, malformed, TestLayout(), "mesh", "bad-schema", &error));
    CHECK_FALSE(error.empty());
    CHECK(draft.mFrontend == "terrain");
    CHECK(Field(draft, "amount")->mLayout.offset == 0);
    CHECK(Field(draft, "amount")->mComponents[0] == 5.0f);
    CHECK(mir::MaterialDraftDirty(draft));
}

TEST_CASE("A schema refresh reports removed and retyped dirty fields and adds new ones", "[editor][material]")
{
    mir::MaterialDraft draft = TestDraft();
    Field(draft, "count")->mSigned = 7;
    Field(draft, "uv")->mComponents = {9.0f, 9.0f, 0.0f, 0.0f};

    mir::MaterialLayout changed;
    changed.fields = {
        {.name = "amount", .kind = mir::MaterialFieldKind::Float, .offset = 0, .size = 4},
        {.name = "count", .kind = mir::MaterialFieldKind::Float, .offset = 4, .size = 4},
        {.name = "index", .kind = mir::MaterialFieldKind::Uint, .offset = 8, .size = 4},
        {.name = "flag", .kind = mir::MaterialFieldKind::Bool, .offset = 12, .size = 4},
        {.name = "color", .kind = mir::MaterialFieldKind::Float3, .offset = 16, .size = 12},
        {.name = "tint", .kind = mir::MaterialFieldKind::Float4, .offset = 32, .size = 16},
        {.name = "extra", .kind = mir::MaterialFieldKind::Float, .offset = 48, .size = 4},
        {.name = "albedo", .kind = mir::MaterialFieldKind::TextureRef, .offset = 56, .size = 8},
        {.name = "macroTextureRef", .kind = mir::MaterialFieldKind::TextureRef, .offset = 64, .size = 4},
    };
    changed.size = 80;

    mir::MaterialRecipe recipe = TestRecipe();
    recipe.parametersJson = R"({
        "amount": 1.0, "count": 2.0, "index": 3, "flag": true,
        "color": [0.1, 0.2, 0.3], "tint": [1.0, 1.0, 1.0, 1.0], "extra": 3.5
    })";

    std::string error;
    const std::optional<mir::MaterialDraftRefresh> refresh =
        mir::RefreshMaterialDraft(draft, recipe, changed, draft.mFrontend, "changed", &error);
    INFO(error);
    REQUIRE(refresh);
    CHECK(refresh->mMessages.size() == 2);
    CHECK_FALSE(mir::MaterialDraftDirty(draft));

    const auto find = [&](std::string_view name) -> const mir::MaterialDraftField *
    {
        const auto found = std::find_if(draft.mFields.begin(), draft.mFields.end(),
            [&](const mir::MaterialDraftField &field) { return field.mLayout.name == name; });
        return found != draft.mFields.end() ? &*found : nullptr;
    };
    REQUIRE(find("count") != nullptr);
    REQUIRE(find("count")->mLayout.kind == mir::MaterialFieldKind::Float);
    CHECK(find("count")->mComponents[0] == 2.0f);
    REQUIRE(find("extra") != nullptr);
    CHECK(find("extra")->mComponents[0] == 3.5f);
    CHECK(find("uv") == nullptr);
}

TEST_CASE("A clean draft refreshes from an external cook without replaying clean values", "[editor][material]")
{
    mir::MaterialDraft first = TestDraft();
    mir::MaterialDraft second = TestDraft();
    second.mIdentity = "games/Test|games/Test/assets/materials/other.material.json";
    second.mLogicalPath = "games/Test/assets/materials/other.material.json";
    Field(second, "amount")->mComponents[0] = 9.0f;

    mir::MaterialRecipe changed = TestRecipe();
    changed.parametersJson = R"({
        "amount": 4.0, "count": 2, "index": 3, "flag": true,
        "color": [0.1, 0.2, 0.3], "tint": [1.0, 1.0, 1.0, 1.0], "uv": [1.0, 2.0],
        "unknownParameter": 7.0
    })";
    std::string error;
    REQUIRE(mir::RefreshMaterialDraft(first, changed, TestLayout(), first.mFrontend, "external", &error));
    REQUIRE(mir::RefreshMaterialDraft(second, TestRecipe(), TestLayout(), second.mFrontend, "external", &error));

    CHECK(Field(first, "amount")->mComponents[0] == 4.0f);
    CHECK_FALSE(mir::MaterialDraftDirty(first));
    CHECK_FALSE(mir::MaterialDraftPreviewParameters(first, true, true, 2).has_value());
    CHECK(Field(second, "amount")->mComponents[0] == 9.0f);
    CHECK(mir::MaterialDraftDirty(second));
    const std::optional<std::string> overlay = mir::MaterialDraftPreviewParameters(second, true, true, 2);
    REQUIRE(overlay);
    CHECK(nlohmann::json::parse(*overlay).at("amount") == 9.0f);
    CHECK_FALSE(nlohmann::json::parse(*overlay).contains("count"));
}

TEST_CASE("A material cook waits for the matching apply and reports late failure", "[editor][material]")
{
    mir::MaterialDraft draft = TestDraft();
    Field(draft, "amount")->mComponents[0] = 5.0f;
    mir::MaterialDraftMarkSaved(draft, 7, 10, true);

    CHECK(mir::ResolveMaterialCook(draft, true, false, {}, true, 6, std::nullopt, 11) == mir::MaterialCookOutcome::Waiting);
    CHECK(mir::ResolveMaterialCook(draft, true, false, {}, true, 7, std::nullopt, 10) == mir::MaterialCookOutcome::Waiting);
    CHECK(mir::ResolveMaterialCook(draft, true, false, {}, true, 7, std::nullopt, 11) == mir::MaterialCookOutcome::Applied);
    CHECK(mir::ResolveMaterialCook(draft, true, false, {}, false, 7, std::nullopt, 10) == mir::MaterialCookOutcome::Applied);
    CHECK(mir::ResolveMaterialCook(draft, false, false, {}, true, 7, std::nullopt, 11) == mir::MaterialCookOutcome::Waiting);
    CHECK(mir::ResolveMaterialCook(draft, true, true, "older batch failed", true, 7, 6, 11) ==
        mir::MaterialCookOutcome::Waiting);
    CHECK(mir::ResolveMaterialCook(draft, true, true, "late apply failed", true, 7, 7, 11) ==
        mir::MaterialCookOutcome::Failed);

    CHECK_FALSE(mir::MaterialDraftCooked(draft, false, "late apply failed"));
    CHECK(draft.mApplyFailed);
    CHECK(mir::MaterialDraftDirty(draft));
    CHECK(Field(draft, "amount")->mComponents[0] == 5.0f);
}

TEST_CASE("An ordinary revert leaves the recipe untouched and restores the accepted value", "[editor][material]")
{
    const std::filesystem::path path = MakeScratchFile();
    const std::string recipeText = R"({"version": 1, "parameters": {"amount": 1.0}})";
    REQUIRE(mir::WriteText(path, recipeText));

    mir::MaterialDraft draft = TestDraft();
    Field(draft, "amount")->mComponents[0] = 5.0f;
    mir::MaterialDraftRevert(draft);
    CHECK(Field(draft, "amount")->mComponents[0] == 1.0f);
    CHECK_FALSE(mir::MaterialDraftDirty(draft));

    const std::optional<std::string> unchanged = mir::ReadText(path);
    REQUIRE(unchanged);
    CHECK(*unchanged == recipeText);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}
TEST_CASE("Accepted material refresh is per asset, not a global generation", "[editor][material]")
{
    mir::MaterialDraft draft = TestDraft();
    const mir::MaterialRecipe recipe = TestRecipe();
    const uint64_t accepted = mir::MaterialAcceptedRevision(recipe.shaderType, recipe.parametersJson,
        recipe.textures, recipe.frontends);
    draft.mAcceptedRevision = accepted;
    draft.mSchemaRevision = "schema";

    CHECK(mir::DecideMaterialAcceptedRefresh(draft, accepted, "schema") == mir::MaterialAcceptedRefresh::None);
    CHECK(mir::DecideMaterialAcceptedRefresh(draft, accepted + 1, "schema") ==
        mir::MaterialAcceptedRefresh::Accepted);
    CHECK(mir::DecideMaterialAcceptedRefresh(draft, accepted, "new-schema") ==
        mir::MaterialAcceptedRefresh::SchemaOnly);

    mir::MaterialRecipe changed = recipe;
    changed.textures[0].path = "textures/other.png";
    CHECK(mir::MaterialAcceptedRevision(changed.shaderType, changed.parametersJson, changed.textures,
              changed.frontends) != accepted);
}

TEST_CASE("An unaccepted raw change keeps the old baseline while another draft stays dirty", "[editor][material]")
{
    mir::MaterialDraft first = TestDraft();
    mir::MaterialDraft second = TestDraft();
    second.mIdentity = "games/Test|games/Test/assets/materials/other.material.json";
    Field(second, "amount")->mComponents[0] = 9.0f;

    const mir::MaterialRecipe recipe = TestRecipe();
    const uint64_t accepted = mir::MaterialAcceptedRevision(recipe.shaderType, recipe.parametersJson,
        recipe.textures, recipe.frontends);
    first.mAcceptedRevision = accepted;
    second.mAcceptedRevision = accepted;
    first.mSchemaRevision = "schema";
    second.mSchemaRevision = "schema";

    CHECK(mir::DecideMaterialAcceptedRefresh(first, accepted, "schema") == mir::MaterialAcceptedRefresh::None);
    CHECK(Field(first, "amount")->mComponents[0] == 1.0f);
    CHECK(mir::MaterialDraftDirty(second));
    CHECK(Field(second, "amount")->mComponents[0] == 9.0f);
}
TEST_CASE("A failed source write can be repaired from the accepted baseline", "[editor][material]")
{
    const std::filesystem::path path = MakeScratchFile();
    REQUIRE(mir::WriteText(path, R"({
        "version": 1,
        "shader": { "file": "materials/test.slang", "type": "TestMaterial" },
        "frontends": ["terrain"],
        "parameters": {
            "amount": 1.0, "count": 2, "index": 3, "flag": true,
            "color": [0.1, 0.2, 0.3], "tint": [1.0, 1.0, 1.0, 1.0], "uv": [1.0, 2.0],
            "unknownParameter": 7.0
        },
        "textures": {
            "baseColor": { "path": "textures/base.png", "colorSpace": "srgb" },
            "ao": { "path": "textures/ao.png", "colorSpace": "srgb" },
            "normal": { "path": "textures/normal.png", "colorSpace": "linear" },
            "roughness": { "path": "textures/rough.png", "colorSpace": "linear" }
        },
        "resolution": 1024,
        "futureRoot": { "kept": true }
    })"));

    std::string error;
    const std::optional<mir::MaterialRecipe> recipe = mir::LoadMaterialRecipe(path, &error);
    INFO(error);
    REQUIRE(recipe);
    std::optional<mir::MaterialDraft> draft = mir::BuildMaterialDraft("games/Test|games/Test/assets/materials/test.material.json",
        "games/Test/assets/materials/test.material.json", "games/Test", "terrain", *recipe, TestLayout(), &error);
    INFO(error);
    REQUIRE(draft);

    Field(*draft, "amount")->mComponents[0] = 5.0f;
    auto base = std::find_if(draft->mTextures.begin(), draft->mTextures.end(),
        [](const mir::MaterialTextureSource &texture) { return texture.mRole == "baseColor"; });
    REQUIRE(base != draft->mTextures.end());
    base->mPath = "textures/missing.png";

    const mir::MaterialDraftSavePatch first = mir::MaterialDraftSavePatchFor(*draft);
    CHECK(nlohmann::json::parse(first.mParametersJson).at("amount") == 5.0f);
    CHECK(first.mTextures.at("baseColor") == "textures/missing.png");
    REQUIRE(mir::SaveMaterialRecipe(path, first.mParametersJson, first.mTextures, &error));
    mir::MaterialDraftMarkSaved(*draft, 1, 10, true);
    REQUIRE_FALSE(mir::MaterialDraftCooked(*draft, false, "late apply failed"));
    CHECK(draft->mApplyFailed);

    Field(*draft, "amount")->mComponents[0] = 1.0f;
    base->mPath = "textures/base.png";
    CHECK_FALSE(mir::MaterialDraftDirty(*draft));
    CHECK(mir::MaterialDraftNeedsSave(*draft));

    const mir::MaterialDraftSavePatch repair = mir::MaterialDraftSavePatchFor(*draft);
    CHECK(nlohmann::json::parse(repair.mParametersJson).at("amount") == 1.0f);
    CHECK(repair.mTextures.at("baseColor") == "textures/base.png");
    REQUIRE(mir::SaveMaterialRecipe(path, repair.mParametersJson, repair.mTextures, &error));

    const std::optional<nlohmann::json> saved = mir::ReadJsonFile(path);
    REQUIRE(saved);
    CHECK(saved->at("parameters").at("amount") == 1.0);
    CHECK(saved->at("parameters").at("unknownParameter") == 7.0);
    CHECK(saved->at("textures").at("baseColor").at("path") == "textures/base.png");
    CHECK(saved->at("textures").at("baseColor").at("colorSpace") == "srgb");
    CHECK(saved->at("resolution") == 1024);
    CHECK(saved->at("futureRoot").at("kept") == true);

    mir::MaterialDraftMarkSaved(*draft, 2, 11, true);
    CHECK(mir::MaterialDraftCooked(*draft, true, {}));
    CHECK_FALSE(mir::MaterialDraftNeedsSave(*draft));
    CHECK_FALSE(draft->mApplyFailed);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST_CASE("Unchanged retry and memory-only Revert keep pending source repair", "[editor][material]")
{
    mir::MaterialDraft draft = TestDraft();
    auto base = std::find_if(draft.mTextures.begin(), draft.mTextures.end(),
        [](const mir::MaterialTextureSource &texture) { return texture.mRole == "baseColor"; });
    REQUIRE(base != draft.mTextures.end());
    base->mPath = "textures/missing.png";
    mir::MaterialDraftMarkSaved(draft, 1, 10, true);
    REQUIRE_FALSE(mir::MaterialDraftCooked(draft, false, "late apply failed"));

    const mir::MaterialDraftSavePatch unchanged = mir::MaterialDraftSavePatchFor(draft);
    CHECK(unchanged.Empty());
    CHECK(mir::MaterialDraftNeedsSave(draft));

    mir::MaterialDraftRevert(draft);
    CHECK(draft.mApplyFailed);
    CHECK(mir::MaterialDraftNeedsSave(draft));
    CHECK(base->mPath == "textures/base.png");
    const mir::MaterialDraftSavePatch repaired = mir::MaterialDraftSavePatchFor(draft);
    CHECK(repaired.mTextures.at("baseColor") == "textures/base.png");
}