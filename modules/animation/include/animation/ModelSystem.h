/**
 * @file ModelSystem.h
 * @author Jackey Cen
 * @brief Binds shared models to entities and evaluates their animation.
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
        // library is the App's one library, every World's ModelSystem takes its models from it
        ModelSystem(std::shared_ptr<ModelLibrary> library, AsyncAssetService *assets, const AssetResolver &resolver,
            JobExecutor *jobs = nullptr);
        // Frees this World's models without releasing resources owned by other Worlds.
        void ReleaseResources();
        void CommitReload();
        void OnUpdate(SystemContext &context) override;
        // Resolves and prepares the scene's models without evaluating them, so a standalone game can wait for
        // them before its gameplay starts.
        void OnLoadingUpdate(SystemContext &context) override;
        // Claims are dropped while the renderer the models name is still there
        void OnStop(SystemContext &) override { ReleaseResources(); }

        // ModelPlayer's OnRemove: drops the entity's request, poses and morph overrides, so a
        // packed id the World hands out again starts fresh
        void Forget(World &world, Entity entity);

    private:
        // Keep drawing the current model while its replacement loads.
        struct WorldModel
        {
            ModelLibrary::Claim current;
            ModelLibrary::Claim next;
        };
        // Returns null while loading, throws on failure.
        const PreparedModel *Load(AssetId id);
        void AdvancePreparations();
        void PruneModels();

        enum class Binding : uint8_t { Pending, Ready, Failed };
        Binding Bind(ModelPlayer &player, uint64_t packed, AssetId &id, std::string &path, const PreparedModel *&model);
        std::shared_ptr<ModelLibrary> mLibrary;
        AsyncAssetService *mAssets;
        const AssetResolver &mResolver;
        JobExecutor *mJobs;
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
