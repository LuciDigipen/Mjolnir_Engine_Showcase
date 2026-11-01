/**
 * @file GroundQuery.cpp
 * @author Jackey Cen
 * @brief World resource for ground heights a module outside the terrain stack can read.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/ecs/GroundQuery.h>

#include <algorithm>
#include <utility>

namespace mir
{
    namespace
    {
        constexpr std::size_t kMaxDiagnosticLength = 160;

        void LimitDiagnostic(std::string &diagnostic)
        {
            if (diagnostic.size() > kMaxDiagnosticLength)
                diagnostic.resize(kMaxDiagnosticLength);
        }
    }

    struct GroundQuery::State
    {
        IGroundQuerySource *source = nullptr;
        uint64_t token = 0;
        uint64_t nextToken = 0;
        uint64_t revision = 1;
    };

    GroundQuery::GroundQuery() : mState(std::make_shared<State>()) {}

    Registration GroundQuery::Bind(IGroundQuerySource &source)
    {
        std::shared_ptr<State> state = mState;
        state->source = &source;
        state->token = ++state->nextToken;
        ++state->revision;

        const uint64_t token = state->token;
        return Registration(state, [state, token]
            {
                if (state->token != token)
                    return;
                state->source = nullptr;
                state->token = 0;
                ++state->revision;
            });
    }

    void GroundQuery::Invalidate()
    {
        ++mState->revision;
    }

    uint64_t GroundQuery::Revision() const
    {
        return mState->revision;
    }

    bool GroundQuery::Bound() const
    {
        return mState->source != nullptr;
    }

    uint64_t GroundQuery::Identity() const
    {
        return mState->source ? mState->source->Identity() : 0;
    }

    GroundQueryResult GroundQuery::Sample(WorldPosition position) const
    {
        if (mState->source == nullptr)
        {
            GroundQueryResult result;
            result.diagnostic = "No ground query source is registered.";
            return result;
        }

        GroundQueryResult result = mState->source->Sample(position);
        LimitDiagnostic(result.diagnostic);
        return result;
    }

    GroundQueryCoverage GroundQuery::Probe(WorldPosition position) const
    {
        if (mState->source == nullptr)
        {
            GroundQueryCoverage coverage;
            coverage.mDiagnostic = "No ground query source is registered.";
            return coverage;
        }

        GroundQueryCoverage coverage = mState->source->Probe(position);
        LimitDiagnostic(coverage.mDiagnostic);
        return coverage;
    }
}
