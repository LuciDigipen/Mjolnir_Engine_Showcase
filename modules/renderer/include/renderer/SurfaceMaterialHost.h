/**
 * @file SurfaceMaterialHost.h
 * @author Jackey Cen
 * @brief The renderer operations a surface material library needs, without a device.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/Material.h>
#include <renderer/Mesh.h>
#include <renderer/SurfaceMaterialCatalog.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace mir
{
    class VulkanRenderer;

    // Production is VulkanRenderer through MakeVulkanSurfaceMaterialHost. Tests implement the same operations
    // to drive the asynchronous lifecycle without a GPU.
    class SurfaceMaterialHost
    {
    public:
        virtual ~SurfaceMaterialHost() = default;

        virtual const SurfaceMaterialCatalog &MaterialCatalog() const = 0;
        virtual std::optional<SurfaceMaterialSchema> LoadSchema(const SurfaceMaterialRegistration &registration,
            std::string *error = nullptr) const = 0;
        virtual MaterialHandle CreateSurfaceMaterial(const MaterialDesc &desc) = 0;
        virtual uint32_t MaterialSamplerRow(const SamplerDesc &sampler) = 0;
        virtual std::optional<MaterialRecordSnapshot> MaterialRecord(MaterialHandle handle) const = 0;
        virtual bool PrepareMaterialRecords(std::span<const MaterialRecordUpdate> updates,
            std::string *error = nullptr) = 0;
        virtual void CommitMaterialRecords() = 0;
        virtual void DiscardMaterialRecords() = 0;
        virtual void ReleaseSurfaceResources(std::span<const MeshHandle> meshes,
            std::span<const MaterialHandle> materials, std::span<const TextureHandle> textures) = 0;
    };

    std::unique_ptr<SurfaceMaterialHost> MakeVulkanSurfaceMaterialHost(VulkanRenderer &renderer);
}
