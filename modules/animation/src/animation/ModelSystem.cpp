/**
 * @file ModelSystem.cpp
 * @author Jackey Cen
 * @brief GPU upload, reload and per-frame evaluation of ModelPlayer instances.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "animation/ModelSystem.h"
#include <animation/ModelPlayer.h>
#include <animation/ModelEvaluation.h>
#include <jobs/JobExecutor.h>
#include <exception>
#include <assets/AssetResolver.h>
#include <cmath>
#include <core/ecs/EngineMode.h>
#include <core/ecs/World.h>
#include <core/ecs/components/WorldTransform.h>
#include <core/log/Log.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <span>
#include <stdexcept>

namespace mir
{
    namespace
    {
        SamplerDesc ToSamplerDesc(const MaterialAsset &material)
        {
            const auto wrap = [](uint32_t gl)
                {
                    return gl == 33071 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                        : gl == 33648 ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
                        : VK_SAMPLER_ADDRESS_MODE_REPEAT;
                };

            SamplerDesc sampler;
            sampler.magFilter = material.magFilter == 9728 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            // NEAREST, NEAREST_MIPMAP_NEAREST, NEAREST_MIPMAP_LINEAR.
            sampler.minFilter = (material.minFilter == 9728 || material.minFilter == 9984 ||
                material.minFilter == 9986)
                ? VK_FILTER_NEAREST
                : VK_FILTER_LINEAR;
// *_MIPMAP_LINEAR.
            sampler.mipmapMode = (material.minFilter == 9986 || material.minFilter == 9987)
                ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sampler.addressModeU = wrap(material.wrapS);
            sampler.addressModeV = wrap(material.wrapT);
            // Only the four *_MIPMAP_* minFilters sample past level 0.
            sampler.useMips = material.minFilter >= 9984;
            return sampler;
        }

        TextureHandle UploadTexture(VulkanRenderer &renderer, AssetCache &assets, AssetId model, AssetId texture)
        {
            TextureAsset tex;
            std::string error;
            if (!LoadTypedAsset(assets, texture, tex, error))
            {
                MIR_LOG_ERROR("Model {:016x}: texture {:016x}: {}, using error texture", model.value, texture.value, error);
                return renderer.ErrorTexture();
            }

            TextureDesc desc{ .width = tex.width,
                .height = tex.height,
                .pixels = std::as_bytes(std::span(tex.rgba)),
                .srgb = tex.format == TextureFormat::Rgba8Srgb,
                .generateMips = tex.mips.empty() };
            for (const auto &mip : tex.mips)
                desc.mips.push_back({ mip.width, mip.height,
                    std::as_bytes(std::span(tex.mipData).subspan(mip.offset, mip.length)) });
            const TextureHandle handle = renderer.CreateTexture(desc);

            if (handle.IsNull())
            {
                MIR_LOG_ERROR("Model {:016x}: texture {:016x} GPU upload failed, using error texture", model.value, texture.value);
                return renderer.ErrorTexture();
            }

            return handle;
        }

        bool ReadMaterial(AssetCache &assets, AssetId model, AssetId material, MaterialAsset &out)
        {
            std::string error;
            if (!LoadTypedAsset(assets, material, out, error))
            {
                MIR_LOG_ERROR("Model {:016x}: material {:016x}: {}, using error texture", model.value, material.value,
                    error);
                return false;
            }
            return true;
        }

    } // namespace
    void ModelSystem::Reset()
    {
        DiscardPreparedReload();
        mModels.clear();
        mTextures.clear();
        mFailed.clear();
        mDeformations.clear();
    }
    void ModelSystem::ReleaseResources()
    {
        Release(mModels, mTextures);
        Reset();
        mRequestedModels.clear();
        mPreviousPoses.clear();
        mCurrentPoses.clear();
    }

    ModelSystem::Loaded ModelSystem::LoadInto(AssetCache &assets, AssetId id,
        std::unordered_map<uint64_t, TextureHandle> &textures)
    {
        Loaded loaded;
        std::string error;
        if (!LoadModelResources(assets, id, loaded.resources, error))
            throw std::runtime_error(error);
        std::unordered_set<uint64_t> existingTextures;
        for (const auto &[textureId, handle] : textures)
            existingTextures.insert(textureId);
        try
        {
            for (const auto &[key, mesh] : loaded.resources.meshes)
            {
                auto &primitives = loaded.meshes[key];
                for (const auto &p : mesh.primitives)
                {
                    std::vector<Vertex> vertices;
                    vertices.reserve(p.vertices.size());
                    for (const auto &v : p.vertices)
                        vertices.push_back({ v.position, glm::vec3(v.color), v.normal, v.uv, v.color.a, v.tangent });
                    MaterialAsset material;
                    const bool materialRead = p.material.IsNull() || ReadMaterial(assets, id, p.material, material);
                    if (materialRead && material.baseColorTexture != p.baseColorTexture)
                        throw std::runtime_error("mesh/material texture reference mismatch");
                    MaterialDesc desc;
                    desc.shaderName = "model";
                    desc.cullMode = material.doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
                    desc.topology = p.topology == MeshTopology::Triangles ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                        : p.topology == MeshTopology::Lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                        : VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                    desc.sampler = ToSamplerDesc(material);
                    desc.alphaMode = static_cast<MaterialAlphaMode>(material.alpha);
                    desc.alphaCutoff = material.alphaCutoff;
                    if (!materialRead)
                        desc.baseColorTexture = mRenderer.ErrorTexture();
                    else if (!material.baseColorTexture.IsNull())
                    {
                        const auto texId = material.baseColorTexture.value;
                        if (!textures.contains(texId))
                        {
                            textures.emplace(texId, UploadTexture(mRenderer, assets, id, material.baseColorTexture));
                        }
                        desc.baseColorTexture = textures.at(texId);
                    }
                    GpuPrimitive gpu;
                    gpu.mesh = mRenderer.CreateMesh(vertices, p.indices);
                    gpu.material = mRenderer.CreateMaterial(desc);
                    desc.frontFace = VK_FRONT_FACE_CLOCKWISE;
                    gpu.mirrored = mRenderer.CreateMaterial(desc);
                    const bool materialFailed = gpu.material.IsNull() || gpu.mirrored.IsNull();
                    if (materialFailed)
                    {
                        MIR_LOG_ERROR("Model {:016x}: '{}' material creation failed, using error material", id.value,
                            desc.shaderName);
                        if (gpu.material.IsNull())
                            gpu.material = mRenderer.ErrorMaterial();
                        if (gpu.mirrored.IsNull())
                            gpu.mirrored = mRenderer.ErrorMaterial();
                    }
                    // Magenta, not the base color, which would tint the error texture and hide the failure.
                    gpu.factor = materialFailed || desc.baseColorTexture == mRenderer.ErrorTexture()
                        ? glm::vec4(1, 0, 1, 1)
                        : material.baseColor;
                    gpu.transparent = !materialFailed && material.alpha == MaterialAlpha::Blend;
                    primitives.push_back(gpu);
                    if (gpu.mesh.IsNull())
                        throw std::runtime_error("GPU mesh creation failed");
                }
            }
        }
        catch (...)
        {
            std::unordered_map<uint64_t, Loaded> failedModels;
            failedModels.emplace(id.value, std::move(loaded));
            std::unordered_map<uint64_t, TextureHandle> failedTextures;
            for (auto it = textures.begin(); it != textures.end();)
                if (!existingTextures.contains(it->first))
                {
                    failedTextures.emplace(*it);
                    it = textures.erase(it);
                }
                else
                    ++it;
            Release(failedModels, failedTextures);
            throw;
        }
        return loaded;
    }

    ModelSystem::Loaded &ModelSystem::Load(AssetId id)
    {
        if (auto it = mModels.find(id.value); it != mModels.end())
            return it->second;
        if (!mAssets)
            throw std::runtime_error("model asset cache unavailable");

        return mModels.emplace(id.value, LoadInto(*mAssets, id, mTextures)).first->second;
    }

    void ModelSystem::Release(std::unordered_map<uint64_t, Loaded> &models,
        std::unordered_map<uint64_t, TextureHandle> &textures)
    {
        std::vector<MeshHandle> meshes;
        std::vector<MaterialHandle> materials;
        std::vector<TextureHandle> textureHandles;
        for (const auto &[id, model] : models)
            for (const auto &[meshId, primitives] : model.meshes)
                for (const auto &primitive : primitives)
                {
                    meshes.push_back(primitive.mesh);
                    materials.push_back(primitive.material);
                    materials.push_back(primitive.mirrored);
                }
        for (const auto &[id, texture] : textures)
            textureHandles.push_back(texture);
        if (!meshes.empty() || !materials.empty() || !textureHandles.empty())
            mRenderer.ReleaseModelResources(meshes, materials, textureHandles);
        models.clear();
        textures.clear();
    }

    bool ModelSystem::PrepareReload(AssetCache &assets, std::string &error)
    {
        DiscardPreparedReload();
        try
        {
            for (const auto &[id, loaded] : mModels)
                mPreparedModels.emplace(id, LoadInto(assets, AssetId{ id }, mPreparedTextures));
            return true;
        }
        catch (const std::exception &exception)
        {
            error = exception.what();
            DiscardPreparedReload();
            return false;
        }
    }

    void ModelSystem::CommitReload(AssetCache *assets)
    {
        Release(mModels, mTextures);
        mModels = std::move(mPreparedModels);
        mTextures = std::move(mPreparedTextures);
        mAssets = assets;
        mFailed.clear();
        mRequestedModels.clear();
    }

    void ModelSystem::DiscardPreparedReload() { Release(mPreparedModels, mPreparedTextures); }

    void ModelSystem::Forget(World &world, Entity entity)
    {
        const uint64_t packed = PackEntity(entity);
        mRequestedModels.erase(packed);
        mFailed.erase(packed);
        mPreviousPoses.erase(packed);
        mCurrentPoses.erase(packed);
        if (auto *overrides = world.TryResource<ModelMorphOverrides>())
            overrides->instances.erase(packed);
    }
    void ModelSystem::OnUpdate(SystemContext &context)
    {
        auto &draws = context.world.Resource<ModelDrawItems>();
        draws.items.clear();
        mDeformations.clear();
        mCurrentPoses.clear();
        auto *overrides = context.world.TryResource<ModelMorphOverrides>();
        std::unordered_set<uint64_t> alive;
        struct Input
        {
            uint64_t entity;
            std::string path;
            const Loaded *loaded;
            ModelEvaluationInput sample;
        };
        struct Result { ModelEvaluationResult value; std::exception_ptr error; };
        std::vector<Input> inputs;
        {
            context.world.GetOrCreateQuery<ModelPlayer, const WorldTransform>().ForEach(
                [&](Entity entity, ModelPlayer &player, const WorldTransform &transform)
                {
                    const auto packed = PackEntity(entity);
                    alive.insert(packed);
                    if (player.ModelName().empty() && player.ModelAsset().IsNull())
                        return;
                    AssetId id = player.ModelAsset();
                    if (id.IsNull())
                    {
                        std::string reason;
                        id = mResolver.Resolve(player.ModelName(), &reason);
                        if (id.IsNull())
                        {
                            if (mFailed.insert(packed).second)
                                MIR_LOG_ERROR("Model '{}': {}", player.ModelName(), reason);
                            mRequestedModels[packed] = 0;
                            return;
                        }
                    }
                    const std::string path = mResolver.PathOf(id);
                    if (mRequestedModels[packed] != id.value)
                    {
                        mRequestedModels[packed] = id.value;
                        mFailed.erase(packed);
                    }
                    if (mFailed.contains(packed))
                        return;
                    try
                    {
                        auto &data = Load(id);
                        if (player.ModelAsset().IsNull())
                            player.SetModelAsset(id);
                        if (player.IsPlaying() && IsSimulating(context.world))
                            player.SetTime(player.Time() + context.dt * player.Speed());
                        uint32_t clip = uint32_t(player.Clip());
                        const auto &animations = data.resources.model.animations;
                        if (!player.AnimationAsset().IsNull())
                        {
                            const auto found = std::find(animations.begin(), animations.end(), player.AnimationAsset());
                            if (found == animations.end())
                            {
                                if (player.IsPlaying())
                                    MIR_LOG_WARN("Model '{}': referenced animation was removed", path);
                                player.SetPlaying(false);
                                clip = UINT32_MAX;
                            }
                            else
                                clip = static_cast<uint32_t>(found - animations.begin());
                        }
                        else if (clip < animations.size())
                            player.SetAnimationAsset(animations[clip]);
                        if (!player.Loops() && clip < data.resources.clips.size())
                            player.SetTime(std::clamp(player.Time(), 0.0f, data.resources.clips[clip].duration));
                        ModelEvaluationInput sample;
                        sample.resources = &data.resources;
                        sample.world = transform.Pose();
                        sample.clip = clip;
                        sample.scene = static_cast<uint32_t>(player.Scene());
                        sample.time = player.Time();
                        sample.loop = player.Loops();
                        if (overrides)
                            if (auto it = overrides->instances.find(packed); it != overrides->instances.end())
                                sample.overrides = it->second;
                        inputs.push_back(Input{packed, path, &data, std::move(sample)});
                    }
                    catch (const std::exception &e)
                    {
                        MIR_LOG_ERROR("Model '{}': {}", path, e.what());
                        mFailed.insert(packed);
                    }
                });
        } // main-thread gather; no query or World access escapes into a job
        std::vector<Result> results(inputs.size());
        {
            const auto evaluate = [&](size_t begin, size_t end)
            {
                for (size_t i = begin; i < end; ++i)
                    try { EvaluateModelInstance(inputs[i].sample, results[i].value); }
                    catch (...) { results[i].error = std::current_exception(); }
            };
            // Keep the conservative threshold; scale ranges to the available pool.
            // Small scenes and previews keep the same kernel inline.
            if (mJobs && mJobs->WorkerCount() > 0 && inputs.size() >= 64)
            {
                const size_t grain = ModelEvaluationGrain(inputs.size(), mJobs->WorkerCount());
                mJobs->ParallelFor(inputs.size(), grain, evaluate);
            }
            else
                evaluate(0, inputs.size());
        } // every job has joined before touching shared state or drawing
        {
            size_t drawCount = 0;
            for (const auto &result : results)
                for (const auto &node : result.value.nodes) drawCount += node.deformations.size();
            mDeformations.reserve(drawCount);
            draws.items.reserve(drawCount);
            for (size_t i = 0; i < inputs.size(); ++i)
            {
                const auto &input = inputs[i];
                auto &result = results[i];
                for (const auto &key : result.value.invalidOverrides)
                    if (overrides)
                        if (auto instance = overrides->instances.find(input.entity); instance != overrides->instances.end())
                            if (instance->second.erase(key))
                                MIR_LOG_WARN("Model '{}': cleared invalid morph override for node '{}'", input.path, key);
                try
                {
                    for (auto &node : result.value.nodes)
                    {
                        const auto mesh = input.loaded->resources.model.nodes[node.index].mesh;
                        const auto &gpu = input.loaded->meshes.at(mesh.value);
                        const WorldPose *previous = nullptr;
                        if (auto entity = mPreviousPoses.find(input.entity); entity != mPreviousPoses.end())
                            if (auto pose = entity->second.find(node.index); pose != entity->second.end())
                                previous = &pose->second;
                        mCurrentPoses[input.entity][node.index] = node.world;
                        for (size_t primitive = 0; primitive < node.deformations.size(); ++primitive)
                        {
                            mDeformations.push_back(std::move(node.deformations[primitive]));
                            const auto &material = gpu.at(primitive);
                            WorldDrawItem item{material.mesh, node.world, node.normalMatrix, material.factor,
                                node.mirrored ? material.mirrored : material.material, mDeformations.back()};
                            item.transparent = material.transparent;
                            if (previous) item.previousPose = *previous;
                            draws.items.push_back(item);
                        }
                    }
                    if (result.error) std::rethrow_exception(result.error);
                }
                catch (const std::exception &error)
                {
                    MIR_LOG_ERROR("Model '{}': {}", input.path, error.what());
                    mFailed.insert(input.entity);
                }
            }
        }
        if (overrides)
            std::erase_if(overrides->instances, [&](const auto &p) { return !alive.contains(p.first); });
        std::erase_if(mFailed, [&](auto key) { return !alive.contains(key); });
        std::erase_if(mRequestedModels, [&](const auto &entry) { return !alive.contains(entry.first); });
        std::swap(mPreviousPoses, mCurrentPoses);
    }
} // namespace mir
