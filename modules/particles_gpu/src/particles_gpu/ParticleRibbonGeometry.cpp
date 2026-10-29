/**
 * @file ParticleRibbonGeometry.cpp
 * @author sumin.park
 * @brief Connected camera-facing strip joins, finite fallbacks and segment sorting.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "particles_gpu/ParticleRibbonGeometry.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace mir
{
    namespace
    {
        glm::vec3 SafeNormal(glm::vec3 v, glm::vec3 fallback)
        {
            const float n = glm::dot(v, v);
            return std::isfinite(n) && n > 1e-12f ? v / std::sqrt(n) : fallback;
        }
        bool Linked(const RibbonPointSubmission &a, const RibbonPointSubmission &b)
        {
            return a.ribbon == b.ribbon && b.link == a.link + 1;
        }
    }
    bool ValidParticleRenderProperties(const ParticleRenderProperties &p)
    {
        return p.alignment<=1 && p.facing<=1 && p.blend<=1 && p.sort<=1 &&
            std::isfinite(p.softDistance) && p.softDistance>=0 && p.softDistance<=1024 &&
            std::isfinite(p.maxDistance) && p.maxDistance>0 && p.maxDistance<=10000000;
    }
    bool ParticleSphereVisible(glm::vec3 center,float radius,const glm::mat4 &viewProjection,float maxDistance)
    {
        if(!std::isfinite(radius) || radius<0 || !std::isfinite(maxDistance) || maxDistance<=0) return false;
        for(int axis=0;axis<3;++axis) if(!std::isfinite(center[axis])) return false;
        const auto clip=viewProjection*glm::vec4(center,1);
        glm::vec4 margin;
        for(int row=0;row<4;++row)
        {
            if(!std::isfinite(clip[row])) return false;
            margin[row]=glm::length(glm::vec3(viewProjection[0][row],viewProjection[1][row],viewProjection[2][row]))*radius;
        }
        return glm::length(center)<=maxDistance+radius && clip.w+margin.w>0 &&
            std::abs(clip.x)<=clip.w+margin.x+margin.w && std::abs(clip.y)<=clip.w+margin.y+margin.w &&
            clip.z>=-margin.z && clip.z<=clip.w+margin.z+margin.w;
    }
    std::vector<RibbonVertex> BuildRibbonGeometry(std::span<const RibbonSubmission> ribbons,
        WorldPosition camera, glm::vec3 cameraRight, uint32_t maxVertices, const glm::mat4 *viewProjection, RibbonGeometryStats *stats)
    {
        struct Segment { RibbonVertex vertices[6]; double distance; };
        std::vector<Segment> segments;
        const auto fallback = SafeNormal(cameraRight, {1, 0, 0});
        for (const auto &ribbon : ribbons)
        {
            const auto &points = ribbon.points;
            std::vector<glm::vec3> centers(points.size()), offsets(points.size());
            std::vector<bool> valid(points.size(), false);
            for (size_t i = 0; i < points.size(); ++i)
            {
                const auto relative = TryCameraRelativeFloat(points[i].position, camera);
                const auto &p = points[i];
                bool finite = std::isfinite(p.width) && p.width >= 0 && std::isfinite(p.textureDistance);
                for (int c = 0; c < 4; ++c) finite &= std::isfinite(p.color[c]);
                if (relative)
                    for (int axis = 0; axis < 3; ++axis) finite &= std::abs((*relative)[axis]) <= 10000000.0f;
                if (relative && finite) { centers[i] = *relative; valid[i] = true; }
            }
            for (size_t i = 0; i < points.size(); ++i)
            {
                if (!valid[i]) continue;
                glm::vec3 tangent(0);
                if (i && valid[i-1] && Linked(points[i-1], points[i]))
                    tangent += SafeNormal(centers[i] - centers[i-1], {0,0,0});
                if (i+1 < points.size() && valid[i+1] && Linked(points[i], points[i+1]))
                    tangent += SafeNormal(centers[i+1] - centers[i], {0,0,0});
                tangent = SafeNormal(tangent, {0,0,1});
                glm::vec3 direction=SafeNormal(-centers[i],{0,0,1});
                if(viewProjection && ribbon.renderer.facing!=0) direction=SafeNormal(centers[i],direction);
                if(viewProjection && ribbon.renderer.facing==0)
                    direction=SafeNormal(glm::vec3((*viewProjection)[0][3],(*viewProjection)[1][3],(*viewProjection)[2][3]),direction);
                auto side = glm::cross(tangent,direction);
                // The least parallel axis keeps end-on views and coincident samples finite.
                if (glm::dot(side, side) < 1e-10f)
                    side = glm::cross(tangent, std::abs(tangent.y) < 0.9f ? glm::vec3(0,1,0) : glm::vec3(1,0,0));
                offsets[i] = SafeNormal(side, fallback) * points[i].width;
            }
            for (size_t i = 1; i < points.size(); ++i)
            {
                if (!valid[i-1] || !valid[i] || !Linked(points[i-1], points[i]) ||
                    glm::length(centers[i] - centers[i-1]) < 1e-6f) continue;
                const auto center=(centers[i-1]+centers[i])*0.5f;
                const float radius=glm::length(centers[i]-centers[i-1])*0.5f+std::max(points[i-1].width,points[i].width);
                if(viewProjection && !ParticleSphereVisible(center,radius,*viewProjection,ribbon.renderer.maxDistance)) { if (stats) ++stats->culled; continue; }
                if (segments.size() >= maxVertices / 6) { if (stats) { ++stats->dropped; continue; } break; }
                const auto &a = points[i-1]; const auto &b = points[i];
                // Subtract a whole repeat, evicting a prefix never resets texture phase.
                const double base = std::floor(a.textureDistance / 8.0) * 8.0;
                const float u0 = float((a.textureDistance - base) / 8.0);
                const float u1 = float((b.textureDistance - base) / 8.0);
                RibbonVertex corners[]{
                    {glm::vec4(centers[i-1] - offsets[i-1], 1), a.color, {u0,-1,0,0}},
                    {glm::vec4(centers[i-1] + offsets[i-1], 1), a.color, {u0, 1,0,0}},
                    {glm::vec4(centers[i] + offsets[i], 1), b.color, {u1, 1,0,0}},
                    {glm::vec4(centers[i] - offsets[i], 1), b.color, {u1,-1,0,0}}};
                Segment segment{{corners[0],corners[1],corners[2],corners[0],corners[2],corners[3]},
                    ribbon.renderer.sort==0 ? double(glm::length(centers[i-1])) + double(glm::length(centers[i])) : -double(segments.size())};
                segments.push_back(segment);
            }
        }
        std::stable_sort(segments.begin(), segments.end(), [](const auto &a, const auto &b) { return a.distance > b.distance; });
        std::vector<RibbonVertex> result;
        result.reserve(segments.size() * 6);
        for (const auto &segment : segments)
            result.insert(result.end(), std::begin(segment.vertices), std::end(segment.vertices));
        return result;
    }
}
