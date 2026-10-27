/**
 * @file RtInstanceInfo.h
 * @author Sumin Park
 * @brief CPU mirror of raytrace.slang's per-instance record.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <volk.h>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mir
{
    // Must match RtInstanceInfo in assets/shaders/raytrace.slang, including its
    // StructuredBuffer ArrayStride. Scalar padding keeps the layout explicit;
    // a float3 here would use a wider alignment.
    struct RtInstanceInfo
    {
        VkDeviceAddress vertexAddress;
        VkDeviceAddress indexAddress;
        glm::vec4 tint;
        glm::vec4 normalCol0;
        glm::vec4 normalCol1;
        glm::vec4 normalCol2;
        uint32_t flags; // kInstance* bits
        uint32_t baseColorTexture;
        uint32_t baseColorSampler;
        uint32_t materialRecord;
        float alphaCutoff;
        float pad0;
        float pad1;
        float pad2;
        glm::mat4 prevModel;
        // Model linear transform for tangent conversion; the normal matrix is
        // inverse-transpose and cannot recover non-uniform scale.
        glm::vec4 modelCol0;
        glm::vec4 modelCol1;
        glm::vec4 modelCol2;
    };

    static_assert(std::is_standard_layout_v<RtInstanceInfo>);
    static_assert(sizeof(RtInstanceInfo) == 224);
    static_assert(offsetof(RtInstanceInfo, alphaCutoff) == 96);
    static_assert(offsetof(RtInstanceInfo, materialRecord) == 92);
    static_assert(offsetof(RtInstanceInfo, prevModel) == 112);
}
