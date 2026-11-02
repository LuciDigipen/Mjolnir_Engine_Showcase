/**
 * @file ModelSystem.h
 * @author Jackey Cen
 * @brief Turns ModelPlayer instances into GPU draws, in SystemPhase::LateUpdate.
 *
 * Claims each referenced model from the renderer-wide ModelLibrary, which prepares it in the
 * background, so loading never blocks a frame and Worlds share one prepared model. An entity draws
 * once its model is published, advances playback while the world is simulating, evaluates the pose
 * and fills the ModelDrawItems resource that RenderSystem draws. LateUpdate runs after Propagate,
 * so WorldTransform is current. A catalog reload claims replacements and keeps drawing the old
 * model until its replacement is usable.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include <animation/ModelAnimation.h>
#include <animation/ModelLibrary.h>
#include <core/ecs/System.h>
#include <renderer/VulkanRenderer.h>
#include <unordered_set>

namespace mir
{
    class AssetResolver;
    class ModelPlayer;
    class AsyncAssetService;
    class JobExecutor;
    class ModelSystem final : public ISystem
    {
    public:
        ModelSystem(VulkanRenderer &renderer, AsyncAssetService *assets, const AssetResolver &resolver,
            JobExecutor *jobs = nullptr);
        // Frees this World's models without releasing resources owned by other Worlds.
        void ReleaseResources();
        // Prepares every loaded model again from the committed catalog. The old models keep drawing
        // until each replacement is usable, and a replacement that fails leaves the old one in place.
        void CommitReload();
        void OnUpdate(SystemContext &context) override;
        // Resolves and prepares the scene's models without evaluating them, so a standalone game can wait for
        // them before its gameplay starts.
        void OnLoadingUpdate(SystemContext &context) override;

        // ModelPlayer's OnRemove: drops the entity's request, poses and morph overrides, so a
        // packed id the World hands out again starts fresh
        void Forget(World &world, Entity entity);

    private:
        // What this World holds of a model: the claim it draws from and, during a reload, the claim that
        // replaces it once usable.
        struct WorldModel
        {
            ModelLibrary::Claim current;
            ModelLibrary::Claim next;
        };
        // The published model, null while its first preparation runs. Throws when that preparation failed.
        const PreparedModel *Load(AssetId id);
        void AdvancePreparations();

        enum class Binding : uint8_t { Pending, Ready, Failed };
        // Resolves what a player names and asks for it. Failed is final until a reload.
        Binding Bind(ModelPlayer &player, uint64_t packed, AssetId &id, std::string &path, const PreparedModel *&model);
        VulkanRenderer &mRenderer;
        AsyncAssetService *mAssets;
        const AssetResolver &mResolver;
        JobExecutor *mJobs;
        std::shared_ptr<ModelLibrary> mLibrary;
        std::unordered_map<uint64_t, WorldModel> mModels;
        std::unordered_set<uint64_t> mFailed;
        std::unordered_map<uint64_t, uint64_t> mRequestedModels;
        std::vector<std::vector<glm::vec4>> mDeformations;
        // by PackEntity, then node
        using NodePoses = std::unordered_map<uint32_t, WorldPose>;
        std::unordered_map<uint64_t, NodePoses> mPreviousPoses;
        std::unordered_map<uint64_t, NodePoses> mCurrentPoses;
    };
} // namespace mir
