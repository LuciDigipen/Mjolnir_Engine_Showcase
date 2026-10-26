#include <animation/ModelEvaluation.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mir
{
    size_t ModelEvaluationGrain(size_t instances, size_t workers)
    {
        if (instances == 0) return 1;
        // Clamp before adding the participating caller; avoid overflow for any input.
        const size_t participants = workers >= instances ? instances : workers + 1;
        return std::clamp(instances / participants / 4, size_t(1), size_t(32));
    }

    void EvaluateModelInstance(const ModelEvaluationInput &input, ModelEvaluationResult &result)
    {
        result = {};

        if (!input.resources) throw std::invalid_argument("model evaluation has no resources");

        const ModelResources &data = *input.resources;

        auto pose = EvaluateModel(data, input.clip, input.time, input.loop);
        for (size_t i = 0; i < pose.size(); ++i)
            if (auto weights = input.overrides.find(data.model.nodes[i].key); weights != input.overrides.end())
            {
                if (weights->second.size() != pose[i].weights.size())
                {
                    result.invalidOverrides.push_back(weights->first);
                    continue;
                }
                for (float value : weights->second)
                    if (!std::isfinite(value)) throw std::runtime_error("non-finite morph override");
                pose[i].weights = weights->second;
            }

        const auto visible = VisibleModelNodes(data.model, input.scene);
        for (size_t i = 0; i < pose.size(); ++i)
        {
            const auto &node = data.model.nodes[i];
            if (!visible[i] || node.mesh.IsNull()) continue;

            const auto world = TryComposePose(input.world, glm::mat3(pose[i].world), glm::vec3(pose[i].world[3]));
            if (!world) continue;

            const float determinant = glm::determinant(world->linear);
            if (std::abs(determinant) < 1e-10f) continue;

            EvaluatedModelNode evaluated;
            evaluated.index = static_cast<uint32_t>(i);
            evaluated.world = *world;
            evaluated.normalMatrix = glm::inverseTranspose(world->linear);
            evaluated.mirrored = determinant < 0;

            const auto matrices = SkinMatrices(data.model, pose, static_cast<uint32_t>(i));
            result.nodes.push_back(std::move(evaluated));
            for (const auto &primitive : data.meshes.at(node.mesh.value).primitives)
                result.nodes.back().deformations.push_back(BuildDeformationData(primitive, pose[i].weights, matrices));
        }
    }
}
