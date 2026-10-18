/**
 * @file Shapes.h
 * @author Sumin Park
 * @brief Built-in primitive geometry
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/Mesh.h>

#include <optional>
#include <string_view>
#include <vector>

namespace mir
{
    // Temporary for sending MeshData -> CreateMesh
    struct MeshData
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
    };

    /**
     * All are authored in right-handed, Y-up space,
     * counter-clockwise seen from outside, unit-sized and centred on the
     * origin (so a Transform's scale reads directly as world-space
     * dimensions) - except MakeCapsule, see its comment.
     */
    MeshData MakeTriangle();
    MeshData MakeQuad();
    MeshData MakeCube();
    MeshData MakeSphere();   // diameter 1
    MeshData MakePlane();    // 1x1, in the XZ plane, normal +Y
    MeshData MakeCylinder(); // diameter 1, height 1
    MeshData MakeCone();     // base diameter 1, height 1, apex on +Y
    MeshData MakeCapsule();

    inline constexpr std::string_view kCubeMeshName = "builtin:cube";
    inline constexpr std::string_view kSphereMeshName = "builtin:sphere";
    inline constexpr std::string_view kPlaneMeshName = "builtin:plane";
    inline constexpr std::string_view kCylinderMeshName = "builtin:cylinder";
    inline constexpr std::string_view kConeMeshName = "builtin:cone";
    inline constexpr std::string_view kCapsuleMeshName = "builtin:capsule";

    // nullopt for any name other than the kXMeshName constants above.
    std::optional<MeshData> MakeBuiltinMesh(std::string_view meshName);
}
