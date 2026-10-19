/**
 * @file Shapes.cpp
 * @author Sumin Park
 * @brief Built-in primitive geometry, as plain data.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "renderer/Shapes.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/trigonometric.hpp>

#include <cmath>

namespace mir
{
    namespace
    {
        constexpr glm::vec3 kWhite{ 1.0f, 1.0f, 1.0f };

        struct Row
        {
            float y;
            float ringRadius;
            float nRadial;
            float nY;
        };

        MeshData BuildShell(const std::vector<Row> &rows, uint32_t segments)
        {
            MeshData mesh;
            mesh.vertices.reserve(rows.size() * segments);
            mesh.indices.reserve((rows.size() - 1) * segments * 6);

            for (const Row &row : rows)
            {
                for (uint32_t j = 0; j < segments; ++j)
                {
                    const float theta = (static_cast<float>(j) / static_cast<float>(segments)) * glm::two_pi<float>();
                    const float cosT = std::cos(theta);
                    const float sinT = std::sin(theta);
                    const glm::vec3 pos{ row.ringRadius * cosT, row.y, row.ringRadius * sinT };
                    const glm::vec3 normal{ row.nRadial * cosT, row.nY, row.nRadial * sinT };
                    mesh.vertices.push_back(Vertex{ pos, kWhite, normal });
                }
            }

            for (uint32_t i = 0; i + 1 < rows.size(); ++i)
            {
                const uint32_t rowBase = i * segments;
                const uint32_t nextBase = (i + 1) * segments;
                for (uint32_t j = 0; j < segments; ++j)
                {
                    const uint32_t jNext = (j + 1) % segments;
                    const uint32_t a = rowBase + j;
                    const uint32_t b = nextBase + j;
                    const uint32_t c = nextBase + jNext;
                    const uint32_t d = rowBase + jNext;
                    mesh.indices.insert(mesh.indices.end(), { a, b, d, b, c, d });
                }
            }

            return mesh;
        }
    }

    MeshData MakeTriangle()
    {
        // Flat, facing +Z
        constexpr glm::vec3 n{ 0.0f, 0.0f, 1.0f };
        return MeshData{
            { { { 0.0f, 0.5f, 0.0f },{ 1.0f, 0.0f, 0.0f }, n },
             { { -0.5f, -0.5f, 0.0f },{ 0.0f, 1.0f, 0.0f }, n },
             { { 0.5f, -0.5f, 0.0f },{ 0.0f, 0.0f, 1.0f }, n } },
            { 0, 1, 2 } };
    }

    MeshData MakeQuad()
    {
        constexpr glm::vec3 n{ 0.0f, 0.0f, 1.0f };
        return MeshData{
            { { { -0.5f, -0.5f, 0.0f },{ 1.0f, 0.0f, 0.0f }, n },
             { { 0.5f, -0.5f, 0.0f },{ 0.0f, 1.0f, 0.0f }, n },
             { { 0.5f, 0.5f, 0.0f },{ 0.0f, 0.0f, 1.0f }, n },
             { { -0.5f, 0.5f, 0.0f },{ 1.0f, 1.0f, 0.0f }, n } },
            { 0, 1, 2, 0, 2, 3 } };
    }

    MeshData MakeCube()
    {
        constexpr float h = 0.5f;

        const glm::vec3 faceColors[6]{
            { 1.0f, 0.2f, 0.2f }, // +Z front
            { 0.2f, 1.0f, 0.2f }, // -Z back
            { 0.2f, 0.2f, 1.0f }, // +X right
            { 1.0f, 1.0f, 0.2f }, // -X left
            { 1.0f, 0.2f, 1.0f }, // +Y top
            { 0.2f, 1.0f, 1.0f }, // -Y bottom
        };

        const glm::vec3 corners[6][4]{
            { { -h, -h, h },{ h, -h, h },{ h, h, h },{ -h, h, h } },     // +Z
            { { h, -h, -h },{ -h, -h, -h },{ -h, h, -h },{ h, h, -h } }, // -Z
            { { h, -h, h },{ h, -h, -h },{ h, h, -h },{ h, h, h } },     // +X
            { { -h, -h, -h },{ -h, -h, h },{ -h, h, h },{ -h, h, -h } }, // -X
            { { -h, h, h },{ h, h, h },{ h, h, -h },{ -h, h, -h } },     // +Y
            { { -h, -h, -h },{ h, -h, -h },{ h, -h, h },{ -h, -h, h } }, // -Y
        };

        // One outward normal per face, same order as corners/faceColors above.
        const glm::vec3 faceNormals[6]{
            { 0.0f, 0.0f, 1.0f },
            { 0.0f, 0.0f, -1.0f },
            { 1.0f, 0.0f, 0.0f },
            { -1.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, -1.0f, 0.0f },
        };

        MeshData mesh;
        mesh.vertices.reserve(24);
        mesh.indices.reserve(36);

        for (uint32_t face = 0; face < 6; ++face)
        {
            const uint32_t base = face * 4;

            for (uint32_t corner = 0; corner < 4; ++corner)
                mesh.vertices.push_back(Vertex{ corners[face][corner], faceColors[face], faceNormals[face] });

            mesh.indices.insert(mesh.indices.end(),
                { base + 0, base + 1, base + 2,
                 base + 0, base + 2, base + 3 });
        }

        return mesh;
    }

    MeshData MakeSphere()
    {
        constexpr float radius = 0.5f;
        constexpr uint32_t segments = 24; // divisible by 4, so +-X/+-Z land exactly on the equator
        constexpr uint32_t rings = 16;

        std::vector<Row> rows;
        rows.reserve(rings + 1);
        for (uint32_t i = 0; i <= rings; ++i)
        {
            const float phi = (static_cast<float>(i) / static_cast<float>(rings)) * glm::pi<float>();
            const float cosPhi = std::cos(phi);
            const float sinPhi = std::sin(phi);
            // phi=0 -> south pole (y=-radius), phi=pi -> north pole (y=+radius)
            rows.push_back(Row{ -radius * cosPhi, radius * sinPhi, sinPhi, -cosPhi });
        }

        return BuildShell(rows, segments);
    }

    MeshData MakePlane()
    {
        constexpr float h = 0.5f;
        constexpr glm::vec3 n{ 0.0f, 1.0f, 0.0f };
        return MeshData{
            { { { -h, 0.0f, h }, kWhite, n },
             { { h, 0.0f, h }, kWhite, n },
             { { h, 0.0f, -h }, kWhite, n },
             { { -h, 0.0f, -h }, kWhite, n } },
            { 0, 1, 2, 0, 2, 3 } };
    }

    MeshData MakeCylinder()
    {
        constexpr float radius = 0.5f;
        constexpr float h = 0.5f;
        constexpr uint32_t segments = 32; // divisible by 4, so +-X/+-Z land exactly on the rim

        MeshData mesh;
        mesh.vertices.resize(static_cast<size_t>(segments) * 4 + 2);
        mesh.indices.reserve(static_cast<size_t>(segments) * 12);

        const uint32_t sideTop = 0;
        const uint32_t sideBottom = segments;
        const uint32_t capTopRing = segments * 2;
        const uint32_t capBottomRing = segments * 3;
        const uint32_t capTopCenter = segments * 4;
        const uint32_t capBottomCenter = capTopCenter + 1;

        mesh.vertices[capTopCenter] = Vertex{ { 0.0f, h, 0.0f }, kWhite,{ 0.0f, 1.0f, 0.0f } };
        mesh.vertices[capBottomCenter] = Vertex{ { 0.0f, -h, 0.0f }, kWhite,{ 0.0f, -1.0f, 0.0f } };

        for (uint32_t j = 0; j < segments; ++j)
        {
            const float theta = (static_cast<float>(j) / static_cast<float>(segments)) * glm::two_pi<float>();
            const float cosT = std::cos(theta);
            const float sinT = std::sin(theta);
            const glm::vec3 radial{ cosT, 0.0f, sinT };
            const glm::vec3 top{ radius * cosT, h, radius * sinT };
            const glm::vec3 bottom{ radius * cosT, -h, radius * sinT };

            mesh.vertices[sideTop + j] = Vertex{ top, kWhite, radial };
            mesh.vertices[sideBottom + j] = Vertex{ bottom, kWhite, radial };
            mesh.vertices[capTopRing + j] = Vertex{ top, kWhite,{ 0.0f, 1.0f, 0.0f } };
            mesh.vertices[capBottomRing + j] = Vertex{ bottom, kWhite,{ 0.0f, -1.0f, 0.0f } };
        }

        for (uint32_t j = 0; j < segments; ++j)
        {
            const uint32_t jNext = (j + 1) % segments;

            const uint32_t c0 = sideBottom + j;
            const uint32_t c1 = sideTop + j;
            const uint32_t c2 = sideTop + jNext;
            const uint32_t c3 = sideBottom + jNext;
            mesh.indices.insert(mesh.indices.end(), { c0, c1, c2, c0, c2, c3 });

            mesh.indices.insert(mesh.indices.end(),
                { capTopCenter, capTopRing + jNext, capTopRing + j });
            mesh.indices.insert(mesh.indices.end(),
                { capBottomCenter, capBottomRing + j, capBottomRing + jNext });
        }

        return mesh;
    }

    MeshData MakeCone()
    {
        constexpr float radius = 0.5f;
        constexpr float h = 0.5f;
        constexpr float height = 2.0f * h;
        constexpr uint32_t segments = 32; // divisible by 4, so +-X/+-Z land exactly on the rim

        MeshData mesh;
        mesh.vertices.resize(static_cast<size_t>(segments) * 3 + 1);
        mesh.indices.reserve(static_cast<size_t>(segments) * 6);

        const uint32_t baseRing = 0;
        const uint32_t apexRing = segments;
        const uint32_t baseCapRing = segments * 2;
        const uint32_t baseCapCenter = segments * 3;

        mesh.vertices[baseCapCenter] = Vertex{ { 0.0f, -h, 0.0f }, kWhite,{ 0.0f, -1.0f, 0.0f } };

        for (uint32_t j = 0; j < segments; ++j)
        {
            const float theta = (static_cast<float>(j) / static_cast<float>(segments)) * glm::two_pi<float>();
            const float cosT = std::cos(theta);
            const float sinT = std::sin(theta);
            const glm::vec3 base{ radius * cosT, -h, radius * sinT };
            // Outward lateral normal (constant along each slant line): the
            // slope tilts it up toward the apex by height/radius.
            const glm::vec3 slant = glm::normalize(glm::vec3{ height * cosT, radius, height * sinT });

            mesh.vertices[baseRing + j] = Vertex{ base, kWhite, slant };
            mesh.vertices[apexRing + j] = Vertex{ { 0.0f, h, 0.0f }, kWhite, slant };
            mesh.vertices[baseCapRing + j] = Vertex{ base, kWhite,{ 0.0f, -1.0f, 0.0f } };
        }

        for (uint32_t j = 0; j < segments; ++j)
        {
            const uint32_t jNext = (j + 1) % segments;
            mesh.indices.insert(mesh.indices.end(), { baseRing + j, apexRing + j, baseRing + jNext });
            mesh.indices.insert(mesh.indices.end(),
                { baseCapCenter, baseCapRing + j, baseCapRing + jNext });
        }

        return mesh;
    }

    MeshData MakeCapsule()
    {
        constexpr float radius = 0.5f;
        constexpr float cylHalf = 0.5f;
        constexpr uint32_t segments = 24; // divisible by 4, so +-X/+-Z land exactly on the equators
        constexpr uint32_t ringsPerHemisphere = 8;
        constexpr float halfPi = glm::half_pi<float>();

        std::vector<Row> rows;
        rows.reserve(ringsPerHemisphere * 2 + 2);

        // bottom pole up through the bottom equator
        for (uint32_t k = 0; k <= ringsPerHemisphere; ++k)
        {
            const float phi = (static_cast<float>(k) / static_cast<float>(ringsPerHemisphere)) * halfPi;
            const float cosPhi = std::cos(phi);
            const float sinPhi = std::sin(phi);
            rows.push_back(Row{ -cylHalf - radius * cosPhi, radius * sinPhi, sinPhi, -cosPhi });
        }

        // top equator up through the top pole (a separate row from the bottom
        // equator above: same radius, but cylHalf above the midline instead
        // of below it)
        for (uint32_t m = 0; m <= ringsPerHemisphere; ++m)
        {
            const float psi = (static_cast<float>(m) / static_cast<float>(ringsPerHemisphere)) * halfPi;
            const float cosPsi = std::cos(psi);
            const float sinPsi = std::sin(psi);
            rows.push_back(Row{ cylHalf + radius * sinPsi, radius * cosPsi, cosPsi, sinPsi });
        }

        return BuildShell(rows, segments);
    }

    std::optional<MeshData> MakeBuiltinMesh(std::string_view meshName)
    {
        if (meshName == kCubeMeshName)
            return MakeCube();
        if (meshName == kSphereMeshName)
            return MakeSphere();
        if (meshName == kPlaneMeshName)
            return MakePlane();
        if (meshName == kCylinderMeshName)
            return MakeCylinder();
        if (meshName == kConeMeshName)
            return MakeCone();
        if (meshName == kCapsuleMeshName)
            return MakeCapsule();
        return std::nullopt;
    }
}
