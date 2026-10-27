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
                              const glm::mat4 &viewProj, std::vector<DrawItem> &out);

    struct GiTraceVolume
    {
        DrawVolume coverage;
        float maxRayDistance = 0.0f;
    };

    // The probe grid plus one grid diagonal of ray reach and one spacing of
    // relocation/bias allowance on every side.
    // nullopt for invalid settings or an unrepresentable world center.
    std::optional<GiTraceVolume> MakeGiTraceVolume(const DdgiSettings &settings);

    // Keep ordinary world draws conservatively (their submission has no mesh
    // bounds); sources collect the union of camera and GI coverage once.
    // Output must not alias worldItems. Prepare has already run on the sources.
    void CollectRayTracingWorldDraws(std::span<const WorldDrawItem> worldItems,
        std::span<IViewDrawSource *const> sources, size_t view, const ViewCamera &camera,
        const GiTraceVolume &gi, std::vector<WorldDrawItem> &out);
}
