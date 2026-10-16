/**
 * @file TerrainRenderMesh.cpp
 * @author Jackey Cen
 * @brief Validates terrain render payload provenance and builds render meshes.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <terrain/TerrainRenderMesh.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace mir
{
    namespace
    {
        TerrainResult<TerrainTileMesh> Refuse(std::string message)
        {
            return {std::nullopt, {TerrainPackErrorCode::InvalidInput, std::move(message)}};
        }
    }

    TerrainResult<TerrainTileMesh> MakeTerrainRenderMesh(const TerrainIndex &index, const TerrainLodSurface &lod,
                                                         const TerrainLodNormals &normals)
    {
        const TerrainTileRecord *source = index.Find(lod.tile);
        if (!source)
            return Refuse("the tile has no final source");
        const auto expectedOrigin = TerrainLocalPosition::FromMeters(
            {index.layout.SamplePosition(index.layout.TileFirstSample(lod.tile.x)), 0.0,
             index.layout.SamplePosition(index.layout.TileFirstSample(lod.tile.z))});
        if (lod.release != index.release || normals.release != index.release)
            return Refuse("LOD or normals belong to a different release");
        if (lod.tile != normals.tile || lod.layer != normals.layer || lod.level != normals.level)
            return Refuse("LOD and normals have different tile, layer or level keys");
        if (lod.layer >= index.layers.size() || lod.layer < source->layer)
            return Refuse("LOD layer is outside the indexed source range");
        if (lod.level != lod.layer - source->layer)
            return Refuse("LOD level does not match its source layer");
        if (lod.sourceChecksum != source->checksum || normals.sourceChecksum != source->checksum)
            return Refuse("LOD or normals refer to a different final source");
        if (!expectedOrigin || lod.origin.Meters() != expectedOrigin->Meters())
            return Refuse("LOD origin does not match its indexed tile");
        if (lod.sourceSpacing != source->sampleSpacing || normals.sourceSpacing != source->sampleSpacing)
            return Refuse("LOD or normals have a different source spacing");
        if (lod.spacing != normals.spacing || lod.spacing != index.layers[lod.layer].SampleSpacing() ||
            lod.spacing < source->sampleSpacing)
            return Refuse("LOD or normal spacing does not match the indexed layer");
        if (lod.positions.empty() || lod.indices.empty() || lod.indices.size() % 3 != 0)
            return Refuse("LOD vertices or triangle indices are empty or incomplete");
        if (lod.positions.size() != normals.normals.size())
            return Refuse("normal vertex count differs from LOD");
        if (lod.topologyHash != normals.topologyHash ||
            lod.topologyHash != TerrainTopologyHash(lod.positions, lod.indices))
            return Refuse("LOD or normal topology hash differs from the mesh");

        const TerrainTileRecord *record = index.Find(lod.tile, lod.layer, TerrainPayloadKind::LodHeight, lod.level);
        const TerrainTileRecord *normalRecord = index.Find(lod.tile, lod.layer, TerrainPayloadKind::Normal, lod.level);
        if (!record || !normalRecord)
            return Refuse("LOD or normal payload is missing from the index");
        if (record->topologyHash != lod.topologyHash || normalRecord->topologyHash != lod.topologyHash)
            return Refuse("indexed LOD or normal topology hash differs");
        if (record->sourceChecksum != lod.sourceChecksum || normalRecord->sourceChecksum != lod.sourceChecksum)
            return Refuse("indexed LOD or normal source checksum differs");
        if (record->vertexCount != lod.positions.size() || normalRecord->vertexCount != normals.normals.size() ||
            record->indexCount != lod.indices.size())
            return Refuse("indexed LOD or normal element counts differ");
        if (!record->maxDeviation || lod.maxDeviation != *record->maxDeviation)
            return Refuse("LOD error bound differs from the index");
        if (Fnv1a64(EncodeTerrainLod(lod)) != record->checksum ||
            Fnv1a64(EncodeTerrainNormals(lod, normals.normals)) != normalRecord->checksum)
            return Refuse("decoded LOD or normal order differs from its indexed payload");

        TerrainTileMesh mesh;
        mesh.release = index.release;
        mesh.tile = lod.tile;
        mesh.origin = lod.origin;
        mesh.samples = {{1, 1}, {0, 0}};
        mesh.boundsMin = glm::vec3(std::numeric_limits<float>::max());
        mesh.boundsMax = glm::vec3(std::numeric_limits<float>::lowest());
        mesh.vertices.reserve(lod.positions.size());
        for (size_t i = 0; i < lod.positions.size(); ++i)
        {
            const glm::vec3 p = lod.positions[i];
            const glm::vec3 n = normals.normals[i];
            const double length = glm::dot(glm::dvec3(n), glm::dvec3(n));
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(length) ||
                std::abs(length - 1.0) > 1e-5)
                return Refuse("LOD positions or normals are invalid");
            mesh.vertices.push_back({p, n});
            mesh.boundsMin = glm::min(mesh.boundsMin, p);
            mesh.boundsMax = glm::max(mesh.boundsMax, p);
        }
        for (uint32_t i : lod.indices)
            if (i >= mesh.vertices.size())
                return Refuse("LOD index is outside its vertices");
        mesh.indices = lod.indices;
        return {std::move(mesh), {}};
    }

    TerrainResult<TerrainTileMesh> ReadTerrainRenderMesh(const TerrainReleaseHandle &release, TileCoord tile,
                                                         std::optional<uint32_t> layer)
    {
        const TerrainTileRecord *source = release.Index().Find(tile);
        if (!source)
            return Refuse("the tile has no final source");
        const uint32_t selected = layer.value_or(source->layer);
        const TileKey key{release.Info().asset, tile};
        auto lod = release.ReadLod(key, selected);
        if (!lod)
            return {std::nullopt, std::move(lod.error)};
        auto normals = release.ReadNormals(key, selected);
        if (!normals)
            return {std::nullopt, std::move(normals.error)};
        return MakeTerrainRenderMesh(release.Index(), *lod.value, *normals.value);
    }
}
