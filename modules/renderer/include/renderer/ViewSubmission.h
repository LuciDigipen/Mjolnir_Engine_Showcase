/**
 * @file ViewSubmission.h
 * @author Jackey Cen
 * @brief Turns world-space draws into one viewport's camera-relative draw items.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "renderer/RenderSettings.h"
#include "renderer/ViewDrawSource.h"
#include "renderer/VulkanRenderer.h"
#include "renderer/components/Camera.h"

#include <core/debug/DebugLines.h>
#include <core/ecs/components/WorldTransform.h>
#include <core/math/WorldCoordinates.h>

#include <glm/mat4x4.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace mir
{
    // The camera's translation is left out: render space puts this camera at
    // the origin, so only its rotation (and any scale) reaches the view.
    glm::mat4 MakeCameraRelativeViewProjection(const WorldPose &camera, const Camera &lens, float aspectRatio);

    // Narrows world positions to float; returns how many draws did not fit.
    size_t BuildViewDrawItems(std::span<const WorldDrawItem> items, WorldPosition viewportCamera,
                              const glm::mat4 &viewProj, std::vector<DrawItem> &out, bool cut = false);

    // Narrows world-space line vertex pairs against one viewport's camera. A pair with an end too far for float
    // is dropped whole, so the output stays pairs.
    void BuildViewDebugLines(std::span<const DebugLineVertex> lines, WorldPosition viewportCamera,
                             std::vector<DebugLinePoint> &out);

    struct GiTraceVolume
    {
        WorldPosition origin;
        // SDK default; probe density does not bound visibility.
        float maxRayDistance = 1e27f;
    };

    // nullopt for invalid settings or an unrepresentable world center.
    std::optional<GiTraceVolume> MakeGiTraceVolume(const DdgiSettings &settings);

    // Include resident geometry for both probe rays and their shadow rays.
    // Output must not alias worldItems. Prepare has already run on the sources.
    void CollectRayTracingWorldDraws(std::span<const WorldDrawItem> worldItems,
        std::span<IViewDrawSource *const> sources, size_t view, std::vector<WorldDrawItem> &out);
}
