/**
 * @file AssetPathField.tests.cpp
 * @author Jackey Cen
 * @brief Tests for editor/AssetPathField.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "EditingFixture.h"

#include <editor/AssetPathField.h>
#include <editor/history/ActiveCameraEdit.h>

#include <core/ecs/ComponentFields.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using mir::AssetPathCandidates;
    using mir::AssetPathExists;
    using mir::AssetPathFitsField;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::FieldDesc;
    using mir::FieldKind;
    using mir::FieldValue;
    using mir::RuntimeFieldDecl;

    struct TempAssets
    {
        std::filesystem::path mRoot;

        TempAssets()
        {
            mRoot = std::filesystem::temp_directory_path() /
                ("asset-path-field-test-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
            std::filesystem::remove_all(mRoot);
        }

        ~TempAssets() { std::filesystem::remove_all(mRoot); }

        void Write(const std::string &relative) const
        {
            const std::filesystem::path path = mRoot / relative;
            std::filesystem::create_directories(path.parent_path());
            std::ofstream(path) << "x";
        }
    };

    const ComponentOps &AssetPathOps()
    {
        static const RuntimeFieldDecl fields[] = { { "config", FieldKind::String } };
        return ComponentRegistry::Instance().RegisterRuntime("EditorAssetPathTest", fields);
    }

    std::string ReadConfig(World &world, Entity entity)
    {
        const ComponentOps &ops = AssetPathOps();
        const FieldDesc *field = ops.FindField("config");
        char value[mir::kFieldStringCapacity]{};
        field->Read(ops.GetComponent(world, entity), value);
        return value;
    }
} // namespace

TEST_CASE("Asset path candidates list only files with the suffix, sorted and with forward slashes",
    "[editor][inspector][asset-path]")
{
    TempAssets assets;
    assets.Write("alpha.config");
    assets.Write("sub/B.CONFIG");
    assets.Write("sub/notes.txt");
    assets.Write("other.config");

    const std::vector<std::string> candidates = AssetPathCandidates(assets.mRoot, ".config");

    CHECK(candidates == std::vector<std::string>{ "alpha.config", "other.config", "sub/B.CONFIG" });
}

TEST_CASE("Asset path candidates survive a missing or unreadable root", "[editor][inspector][asset-path]")
{
    const std::filesystem::path missing =
        std::filesystem::temp_directory_path() / "asset-path-field-missing-root";
    std::filesystem::remove_all(missing);

    CHECK(AssetPathCandidates({}, ".config").empty());
    CHECK(AssetPathCandidates(missing, ".config").empty());
}

TEST_CASE("An asset path fits a String field only with its terminator", "[editor][inspector][asset-path]")
{
    const std::string justFits(mir::kFieldStringCapacity - 1, 'a');
    const std::string tooLong(mir::kFieldStringCapacity, 'a');

    CHECK(AssetPathFitsField(justFits));
    CHECK_FALSE(AssetPathFitsField(tooLong));
}

TEST_CASE("An asset path is missing until its file exists, and empty is always missing",
    "[editor][inspector][asset-path]")
{
    TempAssets assets;
    CHECK_FALSE(AssetPathExists(assets.mRoot, "configs/alpha.config"));
    CHECK_FALSE(AssetPathExists(assets.mRoot, ""));
    CHECK_FALSE(AssetPathExists({}, "configs/alpha.config"));

    assets.Write("configs/alpha.config");
    CHECK(AssetPathExists(assets.mRoot, "configs/alpha.config"));

    std::filesystem::remove(assets.mRoot / "configs/alpha.config");
    CHECK_FALSE(AssetPathExists(assets.mRoot, "configs/alpha.config"));
}

TEST_CASE("Picking an asset path writes the String field and undoes", "[editor][inspector][asset-path]")
{
    Editing e;
    const ComponentOps &ops = AssetPathOps();
    const Entity entity = e.Add("holder");
    ops.AddDefault(e.mWorld, entity);
    CHECK(ReadConfig(e.mWorld, entity).empty());

    const FieldDesc *field = ops.FindField("config");
    FieldValue value;
    value.mField = static_cast<uint32_t>(field - ops.mFields.data());
    const std::string picked = "configs/alpha.config";
    std::memcpy(value.mBytes, picked.c_str(), picked.size() + 1);

    e.Run(mir::MakeFieldEditCommand(e.mWorld, entity, ops, { value }, "", nullptr));
    CHECK(ReadConfig(e.mWorld, entity) == picked);

    e.Undo();
    CHECK(ReadConfig(e.mWorld, entity).empty());
}
