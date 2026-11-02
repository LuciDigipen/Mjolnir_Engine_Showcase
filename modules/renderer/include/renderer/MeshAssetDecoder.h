/**
 * @file MeshAssetDecoder.h
 * @author sumin.park
 * @brief Decodes a cooked mesh JSON asset into vertices, indices and bounds, safe to run on a worker.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetDecoder.h>
#include <renderer/Mesh.h>
#include <renderer/components/MeshBounds.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mir
{
    struct DecodedMesh
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        MeshBounds bounds;
    };

    MeshBounds ComputeMeshBounds(std::span<const Vertex> vertices);

    // Reports a bad asset through error and never logs, the caller owns the message.
    bool DecodeMeshAsset(const AssetBlobView &root, const AssetDependencies &dependencies, DecodedMesh &out,
        std::string &error);

    // The mesh asset's decoder, no dependencies. One shared descriptor so requests for a mesh share one
    // result.
    inline uint64_t DecodedMeshBytes(const DecodedMesh &mesh)
    {
        return sizeof(mesh) + mesh.vertices.capacity() * sizeof(Vertex) + mesh.indices.capacity() * sizeof(uint32_t);
    }
    inline constexpr AssetDecoder<DecodedMesh> kMeshAssetDecoder{ DecodeMeshAsset, {}, DecodedMeshBytes };
}
