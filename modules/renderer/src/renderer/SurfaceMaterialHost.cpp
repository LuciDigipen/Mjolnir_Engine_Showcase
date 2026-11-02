/**
 * @file SurfaceMaterialHost.cpp
 * @author Jackey Cen
 * @brief The Vulkan surface material host adapter.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/SurfaceMaterialHost.h>

#include <renderer/VulkanRenderer.h>

namespace mir
{
    namespace
    {
        class VulkanSurfaceMaterialHost final : public SurfaceMaterialHost
        {
        public:
            explicit VulkanSurfaceMaterialHost(VulkanRenderer &renderer) : mRenderer(renderer) {}

            const SurfaceMaterialCatalog &MaterialCatalog() const override { return mRenderer.SurfaceMaterials(); }
            std::optional<SurfaceMaterialSchema> LoadSchema(const SurfaceMaterialRegistration &registration,
                std::string *error) const override
            {
                return LoadSurfaceMaterialSchema(registration, error);
            }
            MaterialHandle CreateSurfaceMaterial(const MaterialDesc &desc) override
            {
                return mRenderer.CreateMaterial(desc);
            }
            uint32_t MaterialSamplerRow(const SamplerDesc &sampler) override
            {
                return mRenderer.MaterialSamplerRow(sampler);
            }
            std::optional<MaterialRecordSnapshot> MaterialRecord(MaterialHandle handle) const override
            {
                return mRenderer.MaterialRecord(handle);
            }
            bool PrepareMaterialRecords(std::span<const MaterialRecordUpdate> updates, std::string *error) override
            {
                return mRenderer.PrepareMaterialRecords(updates, error);
            }
            void CommitMaterialRecords() override { mRenderer.CommitMaterialRecords(); }
            void DiscardMaterialRecords() override { mRenderer.DiscardMaterialRecords(); }
            void ReleaseSurfaceResources(std::span<const MeshHandle> meshes,
                std::span<const MaterialHandle> materials, std::span<const TextureHandle> textures) override
            {
                mRenderer.ReleaseModelResources(meshes, materials, textures);
            }

        private:
            VulkanRenderer &mRenderer;
        };
    }

    std::unique_ptr<SurfaceMaterialHost> MakeVulkanSurfaceMaterialHost(VulkanRenderer &renderer)
    {
        return std::make_unique<VulkanSurfaceMaterialHost>(renderer);
    }
}
