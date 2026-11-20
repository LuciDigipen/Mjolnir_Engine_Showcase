/**
 * @file TerrainOverlayMesh.cpp
 * @author Jackey Cen
 * @brief Rebuilds a tile's finest-level mesh from the heights an overlay snapshot holds for it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "terrain/TerrainOverlayMesh.h"

#include <terrain/TerrainConstants.h>
#include <terrain/TileAddressing.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace mir
{
    namespace
    {
        using Sources = std::span<const std::shared_ptr<const TileHeights>>;

        // The bake's final surface with the overlay's tiles in place of the release's source blocks.
        class OverlaySurface
        {
          public:
            OverlaySurface(const TerrainIndex &index, const TerrainOverlay &overlay, Sources sources)
                : mIndex(index), mOverlay(overlay), mSources(sources)
            {
            }

            const TileHeights *Tile(TileCoord tile) const
            {
                if (const std::shared_ptr<const TileHeights> *held = mOverlay.Find(tile); held && *held)
                    return held->get();
                for (const std::shared_ptr<const TileHeights> &source : mSources)
                    if (source && source->Tile() == tile)
                        return source.get();
                return nullptr;
            }

            const TerrainLayout &LayoutOf(TileCoord tile) const
            {
                return mIndex.layers[mIndex.Find(tile)->layer];
            }

            double Height(const TileHeights &tile, double x, double z) const
            {
                const TerrainLayout &layout = LayoutOf(tile.Tile());
                const int n = tile.IntervalsPerTile();
                const double ox = layout.SamplePosition(layout.TileFirstSample(tile.Tile().x));
                const double oz = layout.SamplePosition(layout.TileFirstSample(tile.Tile().z));
                // Include stored padding for a clipped outer cell.
                const double ux = std::clamp((x - ox) / layout.SampleSpacing(), 0.0, double(n));
                const double uz = std::clamp((z - oz) / layout.SampleSpacing(), 0.0, double(n));
                const int ix = std::min(int(std::floor(ux)), n - 1), iz = std::min(int(std::floor(uz)), n - 1);
                const double fx = ux - ix, fz = uz - iz;
                auto at = [&](int a, int b) { return double(tile.Samples()[size_t(b) * (n + 1) + a]); };
                const double a = at(ix, iz), c = at(ix + 1, iz + 1);
                // The diagonal is the surface's, never chosen per tile or parity.
                return fz >= fx ? a + fz * (at(ix, iz + 1) - a) + fx * (c - at(ix, iz + 1))
                                : a + fx * (at(ix + 1, iz) - a) + fz * (c - at(ix + 1, iz));
            }

            const TileHeights *Owner(double x, double z, const TileHeights &fallback) const
            {
                const std::optional<TerrainLocalPosition> local = TerrainLocalPosition::FromMeters({x, 0.0, z});
                if (!local)
                    return &fallback;
                const TileAddress address = AddressLocal(mIndex.layout, *local);
                if (address.IsInside())
                    if (const TileHeights *owner = Tile(address.tile))
                        return owner;
                return &fallback;
            }

            double SharedHeight(double x, double z, const TileHeights &fallback) const
            {
                return Height(*Owner(x, z, fallback), x, z);
            }

            glm::vec3 Normal(double x, double z, const TileHeights &fallback) const
            {
                // Incident source grids determine a common, LOD-independent stencil.
                double step = LayoutOf(fallback.Tile()).SampleSpacing();
                const TerrainLayout &ref = mIndex.layout;
                const double size = ref.TileSize();
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int64_t tx = int64_t(fallback.Tile().x) + dx, tz = int64_t(fallback.Tile().z) + dz;
                        if (tx < INT32_MIN || tx > INT32_MAX || tz < INT32_MIN || tz > INT32_MAX)
                            continue;
                        const TileCoord tile{int32_t(tx), int32_t(tz)};
                        if (!mIndex.Find(tile))
                            continue;
                        const double ax = ref.SamplePosition(ref.TileFirstSample(tile.x)),
                                     az = ref.SamplePosition(ref.TileFirstSample(tile.z));
                        const double eps = kTerrainSharedNodeRoundoffFactor * std::numeric_limits<double>::epsilon() *
                                           std::max({1.0, std::abs(x), std::abs(z), size});
                        if (x >= ax - eps && x <= ax + size + eps && z >= az - eps && z <= az + size + eps)
                            step = std::max(step, LayoutOf(tile).SampleSpacing());
                    }
                const TerrainLocalBounds bounds = ref.Bounds();
                const double xl = std::clamp(x - step, bounds.minX, bounds.maxX);
                const double xr = std::clamp(x + step, bounds.minX, bounds.maxX);
                const double zl = std::clamp(z - step, bounds.minZ, bounds.maxZ);
                const double zr = std::clamp(z + step, bounds.minZ, bounds.maxZ);
                const double gx = xr > xl ? (SharedHeight(xr, std::clamp(z, bounds.minZ, bounds.maxZ), fallback) -
                                            SharedHeight(xl, std::clamp(z, bounds.minZ, bounds.maxZ), fallback)) /
                                               (xr - xl)
                                         : 0.0;
                const double gz = zr > zl ? (SharedHeight(std::clamp(x, bounds.minX, bounds.maxX), zr, fallback) -
                                            SharedHeight(std::clamp(x, bounds.minX, bounds.maxX), zl, fallback)) /
                                               (zr - zl)
                                         : 0.0;
                return glm::vec3(glm::normalize(glm::dvec3(-gx, 1.0, -gz)));
            }

          private:
            const TerrainIndex &mIndex;
            const TerrainOverlay &mOverlay;
            Sources mSources;
        };

        TerrainResult<TerrainTileMesh> Refuse(std::string message)
        {
            return {std::nullopt, {TerrainPackErrorCode::InvalidInput, std::move(message)}};
        }
    } // namespace

    std::vector<TileCoord> TerrainOverlaySourceTiles(const TerrainIndex &index, TileCoord tile)
    {
        std::vector<TileCoord> tiles;
        for (int32_t dz = -1; dz <= 1; ++dz)
            for (int32_t dx = -1; dx <= 1; ++dx)
            {
                if (dx == 0 && dz == 0)
                    continue;
                const int64_t x = int64_t(tile.x) + dx, z = int64_t(tile.z) + dz;
                if (x < INT32_MIN || x > INT32_MAX || z < INT32_MIN || z > INT32_MAX)
                    continue;
                const TileCoord neighbour{int32_t(x), int32_t(z)};
                if (index.Find(neighbour))
                    tiles.push_back(neighbour);
            }
        return tiles;
    }

    TerrainResult<TerrainTileMesh> BuildTerrainOverlayTileMesh(const TerrainIndex &index,
                                                               const TerrainReleaseInfo &release, TileCoord tile,
                                                               const TerrainLodSurface &topology,
                                                               const TerrainOverlay &overlay, Sources sources)
    {
        if (release != index.release)
            return Refuse("the overlay mesh was asked for another release's index");
        const TerrainTileRecord *source = index.Find(tile);
        if (!source || source->layer >= index.layers.size())
            return Refuse("the overlay tile has no source in the release");
        const std::shared_ptr<const TileHeights> *held = overlay.Find(tile);
        if (!held || !*held)
            return Refuse("the overlay does not hold the tile");
        const TileHeights &own = **held;
        if (own.Tile() != tile || own.IntervalsPerTile() != index.layers[source->layer].IntervalsPerTile())
            return Refuse("the overlay tile's heights do not match its source in the release");
        if (topology.release != release || topology.tile != tile || topology.layer != source->layer ||
            topology.level != 0)
            return Refuse("the topology is not the tile's level-0 payload of this release");
        if (topology.positions.empty() || topology.indices.empty() || topology.indices.size() % 3 != 0)
            return Refuse("the tile's level-0 payload has no mesh");
        for (const uint32_t indexInMesh : topology.indices)
            if (indexInMesh >= topology.positions.size())
                return Refuse("a level-0 index is outside its vertices");

        const TerrainLayout &ref = index.layout;
        const int64_t firstX = ref.TileFirstSample(tile.x), firstZ = ref.TileFirstSample(tile.z);
        const int64_t total = ref.IntervalsPerTile();
        const OverlaySurface surface(index, overlay, sources);

        TerrainTileMesh mesh;
        mesh.release = release;
        mesh.tile = tile;
        mesh.origin = topology.origin;
        mesh.samples = {{1, 1}, {0, 0}};
        mesh.vertices.reserve(topology.positions.size());
        mesh.boundsMin = glm::vec3(std::numeric_limits<float>::max());
        mesh.boundsMax = glm::vec3(std::numeric_limits<float>::lowest());
        for (const glm::vec3 &position : topology.positions)
        {
            const int64_t x = firstX + std::llround(double(position.x) / ref.SampleSpacing());
            const int64_t z = firstZ + std::llround(double(position.z) / ref.SampleSpacing());
            const double xMeters = ref.SamplePosition(x), zMeters = ref.SamplePosition(z);
            const bool edge = x - firstX == 0 || z - firstZ == 0 || x - firstX == total || z - firstZ == total;
            const double height = edge ? surface.SharedHeight(xMeters, zMeters, own)
                                       : surface.Height(own, xMeters, zMeters);
            if (!std::isfinite(height))
                return Refuse("an overlay mesh height is not finite");
            TerrainMeshVertex vertex;
            vertex.position = {position.x, static_cast<float>(height), position.z};
            vertex.normal = surface.Normal(xMeters, zMeters, own);
            mesh.vertices.push_back(vertex);
            mesh.boundsMin = glm::min(mesh.boundsMin, vertex.position);
            mesh.boundsMax = glm::max(mesh.boundsMax, vertex.position);
        }
        mesh.indices = topology.indices;
        return {std::move(mesh), {}};
    }
} // namespace mir
