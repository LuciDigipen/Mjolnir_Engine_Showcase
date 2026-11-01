/**
 * @file ModelSystem.cpp
 * @author Jackey Cen
 * @brief Model claims, reload replacement and per-frame evaluation of ModelPlayer instances.
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
#include <core/ecs/SceneLoadGroup.h>
#include <core/ecs/World.h>
#include <core/ecs/components/WorldTransform.h>
#include <core/log/Log.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <span>
#include <stdexcept>

namespace mir
{
    void ModelSystem::ReleaseResources()
    {
        // Dropping the claims is the release, the renderer keeps unused records under its residency target.
        mModels.clear();
        mFailed.clear();
        mDeformations.clear();
        mRequestedModels.clear();
        mPreviousPoses.clear();
        mCurrentPoses.clear();
    }

    void ModelSystem::Prepare(Entry &entry, AssetId id)
    {
        entry.failure.clear();
        entry.pending = std::make_unique<ModelPreparation>(mRenderer, mRenderer, mRenderer.ErrorTexture(), mAssets, id,
            mResolver.PathOf(id));
    }

    const PreparedModel *ModelSystem::Load(AssetId id)
    {
        auto found = mModels.find(id.value);
        if (found == mModels.end())
        {
            found = mModels.emplace(id.value, Entry{}).first;
            Prepare(found->second, id);
        }
        const Entry &entry = found->second;
        if (entry.ready)
            return entry.ready.get();
        if (!entry.failure.empty())
            throw std::runtime_error(entry.failure);
        return nullptr;
    }

    void ModelSystem::AdvancePreparations()
    {
        for (auto &[id, entry] : mModels)
        {
            if (!entry.pending)
                continue;
            const ModelPreparation::State state = entry.pending->Advance();
            if (state == ModelPreparation::State::Ready)
            {
                entry.ready = entry.pending->Take();
                entry.pending.reset();
            }
            else if (state == ModelPreparation::State::Failed)
            {
                if (entry.ready)
                    MIR_LOG_WARN("Model {:016x}: replacement was rejected, keeping the previous one: {}", id,
                        entry.pending->Error());
                else
                    entry.failure = entry.pending->Error();
                entry.pending.reset();
            }
        }
    }

    void ModelSystem::CommitReload()
    {
        for (auto &[id, entry] : mModels)
            Prepare(entry, AssetId{ id });
        mFailed.clear();
        mRequestedModels.clear();
    }

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
        AdvancePreparations();
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
            const PreparedModel *loaded;
            ModelEvaluationInput sample;
        };
        struct Result { ModelEvaluationResult value; std::exception_ptr error; };
        std::vector<Input> inputs;
        // The scene needs each model resolved: published, or failed in a way that will not change.
        uint32_t required = 0, settled = 0;
        {
            context.world.GetOrCreateQuery<ModelPlayer, const WorldTransform>().ForEach(
                [&](Entity entity, ModelPlayer &player, const WorldTransform &transform)
                {
                    const auto packed = PackEntity(entity);
                    alive.insert(packed);
                    if (player.ModelName().empty() && player.ModelAsset().IsNull())
                        return;
                    ++required;
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
                            ++settled;
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
                    {
                        ++settled;
                        return;
                    }
                    bool counted = false;
                    try
                    {
                        const PreparedModel *data = Load(id);
                        // hidden until the first preparation settles
                        if (data == nullptr)
                            return;
                        ++settled;
                        counted = true;
                        if (player.ModelAsset().IsNull())
                            player.SetModelAsset(id);
                        if (player.IsPlaying() && IsSimulating(context.world))
                            player.SetTime(player.Time() + context.dt * player.Speed());
                        uint32_t clip = uint32_t(player.Clip());
                        const auto &animations = data->resources->model.animations;
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
                        if (!player.Loops() && clip < data->resources->clips.size())
                            player.SetTime(std::clamp(player.Time(), 0.0f, data->resources->clips[clip].duration));
                        ModelEvaluationInput sample;
                        sample.resources = data->resources.get();
                        sample.world = transform.Pose();
                        sample.clip = clip;
                        sample.scene = static_cast<uint32_t>(player.Scene());
                        sample.time = player.Time();
                        sample.loop = player.Loops();
                        if (overrides)
                            if (auto it = overrides->instances.find(packed); it != overrides->instances.end())
                                sample.overrides = it->second;
                        inputs.push_back(Input{packed, path, data, std::move(sample)});
                    }
                    catch (const std::exception &e)
                    {
                        MIR_LOG_ERROR("Model '{}': {}", path, e.what());
                        mFailed.insert(packed);
                        if (!counted)
                            ++settled;
                    }
                });
            if (SceneLoadGroup *group = context.world.TryResource<SceneLoadGroup>())
                group->Report(this, required, settled);
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
                        const auto mesh = input.loaded->resources->model.nodes[node.index].mesh;
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
