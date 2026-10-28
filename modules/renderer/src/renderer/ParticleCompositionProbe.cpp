/**
 * @file ParticleCompositionProbe.cpp
 * @author sumin.park
 * @brief Checked camera-relative inputs for the composition probe.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "renderer/ParticleCompositionProbe.h"

#include <glm/matrix.hpp>

#include <cmath>

namespace mir
{
    std::optional<ParticleProbeView> BuildParticleProbeView(WorldPosition anchor,
        WorldPosition camera, const glm::mat4 &viewProj, glm::vec4 rect,
        glm::vec2 invExtent, bool rayDistance)
    {
        if (!(rect.z > 0.0f && rect.w > 0.0f && invExtent.x > 0.0f && invExtent.y > 0.0f))
            return std::nullopt;
        const auto offset = TryWorldDifference(anchor, camera);
        if (!offset)
            return std::nullopt;
        const glm::vec3 relative(offset->Meters());
        const glm::mat4 inverse = glm::inverse(viewProj);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (!std::isfinite(inverse[c][r]))
                    return std::nullopt;
        for (int i = 0; i < 4; ++i)
            if (!std::isfinite(rect[i]))
                return std::nullopt;
        if (!std::isfinite(relative.x) || !std::isfinite(relative.y) || !std::isfinite(relative.z) ||
            !std::isfinite(invExtent.x) || !std::isfinite(invExtent.y))
            return std::nullopt;
        return ParticleProbeView{inverse, glm::vec4(relative, 1.0f), rect,
            glm::vec4(invExtent, rayDistance ? 1.0f : 0.0f, 0.0f)};
    }
}
