/**
 * @file ModelSystem.h
 * @author Jackey Cen
 * @brief Turns ModelPlayer instances into GPU draws, in SystemPhase::LateUpdate.
 *
 * Claims each referenced model through a background ModelPreparation, so loading never blocks a
 * frame. An entity draws once its model is published, advances playback while the world is
 * simulating, evaluates the pose and fills the ModelDrawItems resource that RenderSystem draws.
 * LateUpdate runs after Propagate, so WorldTransform is current. A catalog reload prepares
 * replacements in the background and keeps drawing the old model until its replacement is usable.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include <animation/ModelAnimation.h>
#include <animation/ModelPreparation.h>
#include <core/ecs/System.h>
#include <renderer/VulkanRenderer.h>
#include <unordered_set>

namespace mir
{
    class AssetResolver;
    class AsyncAssetService;
    class JobExecutor;
    class ModelSystem final : public ISystem
    {
    public:
        ModelSystem(VulkanRenderer &renderer, AsyncAssetService *assets, const AssetResolver &resolver,
            JobExecutor *jobs = nullptr)
            : mRenderer(renderer), mAssets(assets), mResolver(resolver), mJobs(jobs)
        {}
        // Frees this World's models without releasing resources owned by other Worlds.
        void ReleaseResources();
        // Prepares every loaded model again from the committed catalog. The old models keep drawing
        // until each replacement is usable, and a replacement that fails leaves the old one in place.
        void CommitReload();
        void OnUpdate(SystemContext &context) override;

        // ModelPlayer's OnRemove: drops the entity's request, poses and morph overrides, so a
        // packed id the World hands out again starts fresh
        void Forget(World &world, Entity entity);

    private:
        struct Entry
        {
            std::unique_ptr<PreparedModel> ready;
            std::unique_ptr<ModelPreparation> pending;
            std::string failure;
        };
        // The published model, null while its first preparation runs. Throws when that preparation failed.
        const PreparedModel *Load(AssetId id);
        void Prepare(Entry &entry, AssetId id);
        void AdvancePreparations();
        VulkanRenderer &mRenderer;
        AsyncAssetService *mAssets;
        const AssetResolver &mResolver;
        JobExecutor *mJobs;
        std::unordered_map<uint64_t, Entry> mModels;
        std::unordered_set<uint64_t> mFailed;
        std::unordered_map<uint64_t, uint64_t> mRequestedModels;
        std::vector<std::vector<glm::vec4>> mDeformations;
        // by PackEntity, then node
        using NodePoses = std::unordered_map<uint32_t, WorldPose>;
        std::unordered_map<uint64_t, NodePoses> mPreviousPoses;
        std::unordered_map<uint64_t, NodePoses> mCurrentPoses;
    };
} // namespace mir
