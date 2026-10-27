/**
 * @file ModelEvaluation.h
 * @author Rahul Nair
 * @brief Immutable model evaluation inputs and worker-owned deformation results.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include <animation/ModelAnimation.h>
#include <core/ecs/components/WorldTransform.h>

namespace mir
{
    using ModelMorphWeights = std::unordered_map<std::string, std::vector<float>>;
    struct ModelEvaluationInput
    {
        // Pinned and immutable until the synchronous evaluation group joins.
        const ModelResources *resources = nullptr;
        WorldPose world;
        uint32_t clip = UINT32_MAX, scene = UINT32_MAX;
        float time = 0;
        bool loop = true;
        ModelMorphWeights overrides;
    };
    struct EvaluatedModelNode
    {
        uint32_t index = 0;
        WorldPose world;
        glm::mat3 normalMatrix{ 1 };
        bool mirrored = false;
        // Owned bytes per primitive. Commit moves them into frame-owned storage.
        std::vector<std::vector<glm::vec4>> deformations;
    };
    struct ModelEvaluationResult
    {
        std::vector<EvaluatedModelNode> nodes;
        std::vector<std::string> invalidOverrides;
    };
    // Target four ranges per participating thread, capped at 32 instances.
    // A positive grain is returned even for an empty input or zero workers.
    size_t ModelEvaluationGrain(size_t instances, size_t workers);
    // Pure CPU calculation: no World, asset loading, renderer, shared writes or
    // logging. Invalid override names remain available even if later work throws.
    void EvaluateModelInstance(const ModelEvaluationInput &input, ModelEvaluationResult &result);
}
