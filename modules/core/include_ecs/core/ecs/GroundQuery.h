/**
 * @file GroundQuery.h
 * @author Jackey Cen
 * @brief World resource for ground heights a module outside the terrain stack can read.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/Registration.h>
#include <core/math/WorldCoordinates.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>

namespace mir
{
    enum class GroundQueryStatus : uint8_t
    {
        Ready,
        Loading,
        OutOfBounds,
        Failed,
    };

    // Only Ready has a surface point. spacing is what answered; sourceSpacing is the finest data baked there.
    struct GroundQueryResult
    {
        GroundQueryStatus status = GroundQueryStatus::Failed;
        std::optional<WorldPosition> surface;
        double spacing = std::numeric_limits<double>::quiet_NaN();
        double sourceSpacing = std::numeric_limits<double>::quiet_NaN();
        // Changes when the releases or placements behind the answer change.
        uint64_t identity = 0;
        std::string diagnostic;

        bool IsReady() const
        {
            return status == GroundQueryStatus::Ready && surface.has_value();
        }
    };

    // Layout metadata only: no tile is requested or read.
    struct GroundQueryCoverage
    {
        bool mCovered = false;
        bool mSourceResident = false;
        double mSourceSpacing = std::numeric_limits<double>::quiet_NaN();
        uint64_t mIdentity = 0;
        std::string mDiagnostic;
    };

    // A provider for the World's GroundQuery; terrain_runtime is the one in the engine.
    class IGroundQuerySource
    {
    public:
        virtual ~IGroundQuerySource() = default;

        // Requests the finest source data and answers Ready only once it is what returned the height.
        virtual GroundQueryResult Sample(WorldPosition position) = 0;
        // Coverage and source spacing from layout metadata only; never requests payloads.
        virtual GroundQueryCoverage Probe(WorldPosition position) = 0;
        virtual uint64_t Identity() const = 0;
    };

    // Copyable handle: copies share the binding, and Revision moves when that binding or its terrain changes.
    class GroundQuery
    {
    public:
        GroundQuery();

        // Replaces an earlier source. The token clears this source only while it is still the bound one.
        [[nodiscard]] Registration Bind(IGroundQuerySource &source);

        // A session compares Revision to cancel when the terrain behind it changed.
        void Invalidate();
        uint64_t Revision() const;
        bool Bound() const;
        uint64_t Identity() const;

        GroundQueryResult Sample(WorldPosition position) const;
        GroundQueryCoverage Probe(WorldPosition position) const;

    private:
        struct State;

        std::shared_ptr<State> mState;
    };
}
