/**
 * @file ModelAnimation.h
 * @author Jackey Cen
 * @brief Model resource loading, clip sampling, pose evaluation, skinning and deformation packing.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include <assets/AssetCache.h>
#include <assets/AssetDecoder.h>
#include <assets/ModelAsset.h>
#include <renderer/MeshAssetDecoder.h>
#include <memory>
#include <vector>
#include <unordered_map>

namespace mir
{
    // uploads matches asset.primitives by index. Empty primitives have a null upload.
    struct MeshRecord
    {
        MeshAsset asset;
        std::vector<std::shared_ptr<const DecodedMesh>> uploads;
    };

    // CPU data shared by models and kept alive for animation.
    struct ModelResources
    {
        // Shared with the asset service so its memory is counted once.
        std::shared_ptr<const ModelAsset> model;
        std::unordered_map<uint64_t, std::shared_ptr<const MeshRecord>> meshes;
        std::unordered_map<uint64_t, std::shared_ptr<const SkeletonAsset>> skeletons;
        // in the order of model.animations
        std::vector<std::shared_ptr<const AnimationClipAsset>> clips;
        // Materials the meshes name, an unreadable one is absent and its primitives take the error appearance.
        std::unordered_map<uint64_t, std::shared_ptr<const MaterialAsset>> materials;
    };
    struct NodePose
    {
        glm::vec3 translation{}, scale{1};
        glm::quat rotation{1, 0, 0, 0};
        glm::mat4 world{1};
        std::vector<float> weights;
    };
    bool LoadModelResources(AssetCache &cache, AssetId id, ModelResources &result, std::string &error);
    bool DecodeModelResources(const AssetBlobView &root, const AssetDependencies &dependencies, ModelResources &result,
                              std::string &error);

    // Each decoder reads one asset, allowing models to share dependencies.
    const AssetDecoder<ModelAsset> &ModelRecordDecoder();
    const AssetDecoder<MeshRecord> &MeshRecordDecoder();
    const AssetDecoder<SkeletonAsset> &SkeletonRecordDecoder();
    const AssetDecoder<AnimationClipAsset> &ClipRecordDecoder();
    const AssetDecoder<MaterialAsset> &MaterialRecordDecoder();

    struct ModelDependencies
    {
        std::vector<AssetId> meshes, skeletons, clips;
    };
    ModelDependencies DependenciesOf(const ModelAsset &model);
    std::vector<AssetId> MaterialsOf(const MeshAsset &mesh);
    bool ValidateModelResources(const ModelResources &resources, std::string &error);
    std::vector<float> SampleAnimationTrack(const AnimationTrack &track, float time);
    // Reset to rest pose each sample; deterministic seeking and independent instances.
    std::vector<NodePose> EvaluateModel(const ModelResources &model, uint32_t clip, float time, bool loop);
    void ResolvePoseWorld(const ModelAsset &model, std::vector<NodePose> &pose);
    std::vector<glm::mat4> SkinMatrices(const ModelAsset &model, const std::vector<NodePose> &pose, uint32_t node);
    std::vector<bool> VisibleModelNodes(const ModelAsset &model, uint32_t scene);
    // Reference implementation for validation of the shader's morph-then-skin path.
    std::vector<AssetVertex> DeformPrimitive(const MeshPrimitive &mesh, std::span<const float> weights,
                                             std::span<const glm::mat4> joints);
    // vec4 encoding consumed by model.slang. Kept here alongside the CPU reference.
    std::vector<glm::vec4> BuildDeformationData(const MeshPrimitive &mesh, std::span<const float> weights,
                                                std::span<const glm::mat4> joints);
} // namespace mir
