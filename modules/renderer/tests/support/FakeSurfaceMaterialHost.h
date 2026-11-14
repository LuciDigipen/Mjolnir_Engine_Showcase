/**
 * @file FakeSurfaceMaterialHost.h
 * @author Jackey Cen
 * @brief Controlled material creation and record publication for surface library tests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/SurfaceMaterialHost.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace mir::test
{
    class FakeSurfaceMaterialHost final : public SurfaceMaterialHost
    {
    public:
        const SurfaceMaterialCatalog &MaterialCatalog() const override { return catalog; }
        std::optional<SurfaceMaterialSchema> LoadSchema(const SurfaceMaterialRegistration &registration,
            std::string *error) const override
        {
            if (const auto found = stemLayouts.find(registration.stem); found != stemLayouts.end())
                return SurfaceMaterialSchema{ found->second, schemaRevision };
            if (!schemaLayout)
            {
                if (error != nullptr)
                    *error = "the fake host has no schema";
                return std::nullopt;
            }
            return SurfaceMaterialSchema{ *schemaLayout, schemaRevision };
        }
        MaterialHandle CreateSurfaceMaterial(const MaterialDesc &desc) override
        {
            if (rejectMaterials || (!rejectStem.empty() && desc.shaderName == rejectStem))
                return {};
            const MaterialHandle handle{ mNextMaterial++, 1 };
            createdMaterials.push_back(desc);
            MaterialRecordSnapshot snapshot;
            snapshot.mMaterial = handle;
            if (const auto found = stemLayouts.find(desc.shaderName); found != stemLayouts.end())
                snapshot.mLayout = found->second;
            else
                snapshot.mLayout = schemaLayout.value_or(MaterialLayout{});
            snapshot.mBytes.assign(desc.record.begin(), desc.record.end());
            records[handle.mIndex] = std::move(snapshot);
            return handle;
        }
        uint32_t MaterialSamplerRow(const SamplerDesc &) override { return 7; }
        std::optional<MaterialRecordSnapshot> MaterialRecord(MaterialHandle handle) const override
        {
            const auto found = records.find(handle.mIndex);
            if (found == records.end())
                return std::nullopt;
            return found->second;
        }
        bool PrepareMaterialRecords(std::span<const MaterialRecordUpdate> updates, std::string *error) override
        {
            if (rejectRecords)
            {
                if (error != nullptr)
                    *error = "the fake host rejected the record batch";
                return false;
            }
            pending.clear();
            for (const MaterialRecordUpdate &update : updates)
            {
                if (!records.contains(update.mMaterial.mIndex))
                {
                    if (error != nullptr)
                        *error = "the record names an unknown material";
                    return false;
                }
                pending.push_back({ update.mMaterial, std::vector<std::byte>(update.mBytes.begin(), update.mBytes.end()) });
            }
            return true;
        }
        void CommitMaterialRecords() override
        {
            for (auto &[material, bytes] : pending)
                records.at(material.mIndex).mBytes = std::move(bytes);
            pending.clear();
        }
        void DiscardMaterialRecords() override { pending.clear(); }
        void ReleaseSurfaceResources(std::span<const MeshHandle> meshes,
            std::span<const MaterialHandle> materials, std::span<const TextureHandle> textures) override
        {
            releasedMeshes.insert(releasedMeshes.end(), meshes.begin(), meshes.end());
            releasedMaterials.insert(releasedMaterials.end(), materials.begin(), materials.end());
            releasedTextures.insert(releasedTextures.end(), textures.begin(), textures.end());
        }

        SurfaceMaterialCatalog catalog;
        std::optional<MaterialLayout> schemaLayout;
        std::unordered_map<std::string, MaterialLayout> stemLayouts;
        std::string schemaRevision = "layout-1";
        bool rejectMaterials = false;
        bool rejectRecords = false;
        std::string rejectStem;
        std::vector<MaterialDesc> createdMaterials;
        std::unordered_map<uint32_t, MaterialRecordSnapshot> records;
        std::vector<MaterialHandle> releasedMaterials;
        std::vector<TextureHandle> releasedTextures;
        std::vector<MeshHandle> releasedMeshes;

    private:
        std::vector<std::pair<MaterialHandle, std::vector<std::byte>>> pending;
        uint32_t mNextMaterial = 0;
    };
}
