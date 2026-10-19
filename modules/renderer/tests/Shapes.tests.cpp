/**
 * @file Shapes.tests.cpp
 * @brief Tests for renderer/Shapes - the builtin primitive geometry editor
 * Create menu primitives resolve to (see MeshResolveSystem in app).
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/Shapes.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/common.hpp>
#include <glm/vec3.hpp>
#include <glm/vector_relational.hpp>

#include <limits>

namespace
{
    using mir::MeshData;

    struct Bounds
    {
        glm::vec3 mMin;
        glm::vec3 mMax;
    };

    Bounds LocalBounds(const MeshData &mesh)
    {
        glm::vec3 min(std::numeric_limits<float>::max());
        glm::vec3 max(std::numeric_limits<float>::lowest());
        for (const mir::Vertex &vertex : mesh.vertices)
        {
            min = glm::min(min, vertex.pos);
            max = glm::max(max, vertex.pos);
        }
        return { min, max };
    }

    // Every index names a real vertex, and every triangle has 3.
    bool IndicesAreValid(const MeshData &mesh)
    {
        if (mesh.indices.size() % 3 != 0)
            return false;
        for (const uint32_t index : mesh.indices)
        {
            if (index >= mesh.vertices.size())
                return false;
        }
        return true;
    }

    bool CloseTo(const glm::vec3 &a, const glm::vec3 &b, float epsilon = 1e-5f)
    {
        return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(epsilon)));
    }
}

TEST_CASE("MakeSphere is a unit-diameter sphere centred on the origin", "[renderer]")
{
    const MeshData mesh = mir::MakeSphere();
    REQUIRE(IndicesAreValid(mesh));

    const Bounds bounds = LocalBounds(mesh);
    CHECK(CloseTo(bounds.mMin, glm::vec3(-0.5f)));
    CHECK(CloseTo(bounds.mMax, glm::vec3(0.5f)));
}

TEST_CASE("MakePlane is a unit quad in the XZ plane, facing +Y", "[renderer]")
{
    const MeshData mesh = mir::MakePlane();
    REQUIRE(IndicesAreValid(mesh));
    CHECK(mesh.vertices.size() == 4);
    CHECK(mesh.indices.size() == 6);

    const Bounds bounds = LocalBounds(mesh);
    CHECK(CloseTo(bounds.mMin, glm::vec3(-0.5f, 0.0f, -0.5f)));
    CHECK(CloseTo(bounds.mMax, glm::vec3(0.5f, 0.0f, 0.5f)));

    for (const mir::Vertex &vertex : mesh.vertices)
        CHECK(CloseTo(vertex.normal, glm::vec3(0.0f, 1.0f, 0.0f)));
}

TEST_CASE("MakeCylinder is a unit-diameter, unit-height cylinder centred on the origin", "[renderer]")
{
    const MeshData mesh = mir::MakeCylinder();
    REQUIRE(IndicesAreValid(mesh));

    const Bounds bounds = LocalBounds(mesh);
    CHECK(CloseTo(bounds.mMin, glm::vec3(-0.5f)));
    CHECK(CloseTo(bounds.mMax, glm::vec3(0.5f)));
}

TEST_CASE("MakeCone is a unit-diameter, unit-height cone centred on the origin", "[renderer]")
{
    const MeshData mesh = mir::MakeCone();
    REQUIRE(IndicesAreValid(mesh));

    const Bounds bounds = LocalBounds(mesh);
    CHECK(CloseTo(bounds.mMin, glm::vec3(-0.5f)));
    CHECK(CloseTo(bounds.mMax, glm::vec3(0.5f)));
}

TEST_CASE("MakeCapsule is a unit-diameter, two-unit-tall capsule centred on the origin", "[renderer]")
{
    const MeshData mesh = mir::MakeCapsule();
    REQUIRE(IndicesAreValid(mesh));

    const Bounds bounds = LocalBounds(mesh);
    CHECK(CloseTo(bounds.mMin, glm::vec3(-0.5f, -1.0f, -0.5f)));
    CHECK(CloseTo(bounds.mMax, glm::vec3(0.5f, 1.0f, 0.5f)));
}

TEST_CASE("MakeBuiltinMesh dispatches every reserved primitive name", "[renderer]")
{
    CHECK(mir::MakeBuiltinMesh(mir::kCubeMeshName));
    CHECK(mir::MakeBuiltinMesh(mir::kSphereMeshName));
    CHECK(mir::MakeBuiltinMesh(mir::kPlaneMeshName));
    CHECK(mir::MakeBuiltinMesh(mir::kCylinderMeshName));
    CHECK(mir::MakeBuiltinMesh(mir::kConeMeshName));
    CHECK(mir::MakeBuiltinMesh(mir::kCapsuleMeshName));
}

TEST_CASE("MakeBuiltinMesh leaves non-builtin names to the asset-file fallback", "[renderer]")
{
    CHECK_FALSE(mir::MakeBuiltinMesh("meshes/cube.mesh.json"));
    CHECK_FALSE(mir::MakeBuiltinMesh(""));
}
