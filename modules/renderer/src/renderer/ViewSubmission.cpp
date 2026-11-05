/**
 * @file ViewSubmission.cpp
 * @author Jackey Cen
 * @brief Turns world-space draws into one viewport's camera-relative draw items.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "renderer/ViewSubmission.h"

#include "renderer/CameraMath.h"

#include <glm/common.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace mir
{
    namespace
    {
        // Translation plus vertex stays below 2^24 steps, which float holds exactly.
        constexpr double kExactGridSteps = 8388608.0;

        std::optional<glm::mat4> GridMatrixRelativeTo(const WorldPose &pose, WorldPosition camera, float grid)
        {
            const std::optional<WorldOffset> offset = TryWorldDifference(pose.position, camera);
            if (!offset)
                return std::nullopt;
            const glm::dvec3 steps = glm::dvec3(offset->Meters()) / double(grid);
            if (!(std::abs(steps.x) <= kExactGridSteps && std::abs(steps.y) <= kExactGridSteps &&
                std::abs(steps.z) <= kExactGridSteps))
                return pose.TryMatrixRelativeTo(camera);

            glm::mat4 m(pose.linear);
            m[3] = glm::vec4(glm::vec3(glm::round(steps) * double(grid)), 1.0f);
            return m;
        }
    }

    glm::mat4 MakeCameraRelativeViewProjection(const WorldPose &camera, const Camera &lens, float aspectRatio)
    {
        return MakeViewProjection(glm::mat4(camera.linear), lens, aspectRatio);
    }

    std::optional<GiTraceVolume> MakeGiTraceVolume(const DdgiSettings &settings)
    {
        if (settings.ValidationError() != nullptr)
            return std::nullopt;
        const auto center = WorldPosition::FromDoubleMeters(
            glm::dvec3(settings.origin[0], settings.origin[1], settings.origin[2]));
        if (!center)
            return std::nullopt;
        return GiTraceVolume{ *center };
    }

    void CollectRayTracingWorldDraws(std::span<const WorldDrawItem> worldItems,
        std::span<IViewDrawSource *const> sources, size_t view, std::vector<WorldDrawItem> &out)
    {
        out.assign(worldItems.begin(), worldItems.end());
        for (IViewDrawSource *source : sources)
            source->AppendRayTracingDraws(view, out);
    }

    void BuildGiVolumeLines(const DdgiSettings &settings, std::vector<DebugLinePoint> &out,
        std::span<const GiProbeDebug> probes)
    {
        out.clear();
        if (settings.ValidationError()) return;
        const auto extent = settings.Extent();
        const glm::vec3 half = glm::vec3(extent[0], extent[1], extent[2]) * 0.5f;
        const auto line = [&](glm::vec3 a, glm::vec3 b, uint32_t color)
        { out.push_back({ a, color }); out.push_back({ b, color }); };
        for (int corner = 0; corner < 8; ++corner)
        {
            glm::vec3 a(0.0f);
            for (int axis = 0; axis < 3; ++axis) a[axis] += (corner & (1 << axis)) ? half[axis] : -half[axis];
            for (int axis = 0; axis < 3; ++axis)
                if (!(corner & (1 << axis)))
                {
                    auto b = a; b[axis] += 2 * half[axis];
                    line(a, b, 0xFF40FF40u);
                }
        }
        uint32_t index = 0;
        for (int y = 0; y < settings.probeCounts[1]; ++y)
            for (int z = 0; z < settings.probeCounts[2]; ++z)
                for (int x = 0; x < settings.probeCounts[0]; ++x)
                {
                    auto p = -half + glm::vec3(x, y, z) * settings.probeSpacing;
                    uint32_t color = 0xFF40FFFFu;
                    if (probes.size() == settings.ProbeCount())
                    {
                        const auto &probe = probes[index];
                        p += glm::vec3(probe.offset[0], probe.offset[1], probe.offset[2]);
                        color = probe.inactive ? 0xFF808080u : 0xFF40FF40u;
                    }
                    ++index;
                    const float r = settings.probeSpacing * 0.04f;
                    line(p - glm::vec3(r, 0, 0), p + glm::vec3(r, 0, 0), color);
                    line(p - glm::vec3(0, r, 0), p + glm::vec3(0, r, 0), color);
                }
    }

    void AppendGiVolumeLines(const DdgiSettings &settings, WorldPosition camera,
        std::span<const DebugLinePoint> volumeLines, std::vector<DebugLinePoint> &out)
    {
        if (volumeLines.empty()) return;
        const auto volume = MakeGiTraceVolume(settings);
        const auto centre = volume ? TryCameraRelativeFloat(volume->origin, camera) : std::nullopt;
        if (!centre) return;
        out.reserve(out.size() + volumeLines.size());
        for (const DebugLinePoint &point : volumeLines)
            out.push_back({ point.position + *centre, point.color });
    }

    void BuildViewDebugLines(std::span<const DebugLineVertex> lines, WorldPosition viewportCamera,
                             std::vector<DebugLinePoint> &out)
    {
        out.clear();
        out.reserve(lines.size());
        for (size_t i = 0; i + 1 < lines.size(); i += 2)
        {
            const std::optional<glm::vec3> from = TryCameraRelativeFloat(lines[i].position, viewportCamera);
            const std::optional<glm::vec3> to = TryCameraRelativeFloat(lines[i + 1].position, viewportCamera);
            if (!from || !to)
                continue;
            out.push_back({ *from, lines[i].color });
            out.push_back({ *to, lines[i + 1].color });
        }
    }

    size_t BuildViewDrawItems(std::span<const WorldDrawItem> items, WorldPosition viewportCamera,
        const glm::mat4 &viewProj, std::vector<DrawItem> &out, bool cut)
    {
        out.clear();
        out.reserve(items.size());

        size_t skipped = 0;
        for (const WorldDrawItem &item : items)
        {
            const std::optional<glm::mat4> model = item.translationGrid > 0.0f
                ? GridMatrixRelativeTo(item.pose, viewportCamera,
                    item.translationGrid)
                : item.pose.TryMatrixRelativeTo(viewportCamera);
            if (!model)
            {
                ++skipped;
                continue;
            }

            const WorldPose &previousPose = cut ? item.pose : item.previousPose.value_or(item.pose);
            const std::optional<glm::mat4> previousModel = item.translationGrid > 0.0f
                ? GridMatrixRelativeTo(previousPose, viewportCamera,
                    item.translationGrid)
                : previousPose.TryMatrixRelativeTo(viewportCamera);

            out.push_back(DrawItem{ item.mesh, *model, item.normalMatrix, item.tint, item.material,
                item.deformation, item.transparent, previousModel.value_or(*model),
                cut || item.previousDeformation.empty() ? item.deformation : item.previousDeformation });
        }

        std::stable_sort(out.begin(), out.end(),
            [&viewProj](const DrawItem &a, const DrawItem &b)
            {
                if (a.transparent != b.transparent)
                    return !a.transparent;
                if (!a.transparent)
                    return false;
                // Clip w is the view depth: farther first, whatever the depth
                // mapping does with z.
                const glm::vec4 pa = viewProj * a.model * glm::vec4(0, 0, 0, 1);
                const glm::vec4 pb = viewProj * b.model * glm::vec4(0, 0, 0, 1);
                return pa.w > pb.w;
            });

        return skipped;
    }
}
