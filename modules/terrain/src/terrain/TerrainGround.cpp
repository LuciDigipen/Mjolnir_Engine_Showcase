/**
 * @file TerrainGround.cpp
 * @author Jackey Cen
 * @brief Ground heights at the finest baked source precision, for the editor's snap.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain/TerrainGround.h>

#include <terrain/TerrainScene.h>
#include <terrain/TileAddressing.h>

#include <cmath>
#include <limits>
#include <string>

namespace mir
{
    namespace
    {
        // Below every layout spacing, so HeightAt targets the finest layer baked at the point.
        constexpr double kSourcePrecisionRequest = std::numeric_limits<double>::min();

        std::string DescribeFailure(TerrainHeightFailure failure)
        {
            switch (failure)
            {
            case TerrainHeightFailure::WrongRelease:
                return "The point does not belong to the loaded terrain release.";
            case TerrainHeightFailure::InvalidInput:
                return "The terrain refused the query point.";
            case TerrainHeightFailure::LoadFailed:
                return "The terrain source tile could not be read.";
            case TerrainHeightFailure::InvalidSamples:
                return "The terrain source tile has invalid samples.";
            case TerrainHeightFailure::None:
                break;
            }
            return "The terrain height query failed.";
        }
    }

    GroundQueryResult ProbeTerrainHeightResult(const TerrainHeightResult &result)
    {
        GroundQueryResult out;
        out.identity = result.release.release.value;
        out.spacing = result.spacing;
        out.sourceSpacing = result.sourceSpacing;

        switch (result.status)
        {
        case TerrainHeightStatus::OutsideMap:
            out.status = GroundQueryStatus::OutOfBounds;
            out.diagnostic = "No terrain covers this point.";
            return out;
        case TerrainHeightStatus::Failed:
            out.status = GroundQueryStatus::Failed;
            out.diagnostic = DescribeFailure(result.failure);
            return out;
        case TerrainHeightStatus::Loading:
            out.status = GroundQueryStatus::Loading;
            out.diagnostic = "Waiting for the terrain source tile.";
            return out;
        case TerrainHeightStatus::MeetsRequirement:
        case TerrainHeightStatus::CoarserWhileLoading:
        case TerrainHeightStatus::CoarserAtSourceLimit:
            break;
        }

        if (!result.worldPoint || !std::isfinite(result.spacing) || !std::isfinite(result.sourceSpacing))
        {
            out.status = GroundQueryStatus::Failed;
            out.diagnostic = "The terrain answer has no usable source spacing.";
            return out;
        }

        // CoarserAtSourceLimit also reports a coarser resident layer when the request is finer than the
        // source, so the status alone never says the source answered.
        if (result.spacing <= result.sourceSpacing)
        {
            out.status = GroundQueryStatus::Ready;
            out.surface = result.worldPoint;
            return out;
        }

        out.status = GroundQueryStatus::Loading;
        out.diagnostic = "Waiting for the terrain source tile.";
        return out;
    }

    GroundQueryResult ProbeTerrainGround(TerrainWorld &world, WorldPosition position)
    {
        return ProbeTerrainHeightResult(world.HeightAt(position, kSourcePrecisionRequest));
    }

    GroundQueryCoverage ProbeTerrainGroundCoverage(TerrainWorld &world, WorldPosition position)
    {
        for (const TerrainInstanceState &instance : world.Instances())
        {
            const TileAddress address =
                AddressWorld(instance.release->Layout(), instance.placement, position);
            if (!address.IsInside())
                continue;

            const TerrainTileRecord *source = instance.release->Handle().Index().Find(address.tile);
            if (source == nullptr)
                continue;

            GroundQueryCoverage coverage;
            coverage.mCovered = true;
            coverage.mSourceSpacing = source->sampleSpacing;
            coverage.mIdentity = instance.release->Info().release.value;

            const TerrainHeightQuery &query = instance.release->Query();
            for (uint32_t layer = 0; layer < query.LayerCount(); ++layer)
            {
                if (query.Layer(layer).layout.SampleSpacing() != source->sampleSpacing)
                    continue;
                coverage.mSourceResident =
                    query.Residency(layer, address.tile) == TerrainTileResidency::Resident;
                break;
            }
            if (!coverage.mSourceResident)
                coverage.mDiagnostic = "Waiting for the terrain source data.";
            return coverage;
        }

        GroundQueryCoverage coverage;
        coverage.mDiagnostic = "No terrain covers this point.";
        return coverage;
    }
}
