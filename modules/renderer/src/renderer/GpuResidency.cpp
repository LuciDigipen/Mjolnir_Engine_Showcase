/**
 * @file GpuResidency.cpp
 * @author sumin.park
 * @brief Held and retiring byte accounting against the renderer-wide target.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "renderer/GpuResidency.h"

#include <algorithm>

namespace mir
{
    uint32_t GpuResidency::AddHolder(GpuCategory category, Source held)
    {
        const uint32_t token = mNextToken++;
        mHolders.push_back({ token, category, std::move(held) });
        return token;
    }

    void GpuResidency::RemoveHolder(uint32_t token)
    {
        std::erase_if(mHolders, [token](const Holder &holder) { return holder.token == token; });
    }

    uint64_t GpuResidency::Held(GpuCategory category) const
    {
        uint64_t bytes = 0;
        for (const Holder &holder : mHolders)
            if (holder.category == category)
                bytes += holder.held();
        return bytes;
    }

    uint64_t GpuResidency::Retiring(GpuCategory category) const
    {
        const Source &source = mRetiring[Index(category)];
        return source ? source() : 0;
    }

    uint64_t GpuResidency::UsedTotal() const
    {
        uint64_t bytes = 0;
        for (size_t category = 0; category < static_cast<size_t>(GpuCategory::Count); ++category)
            bytes += Used(static_cast<GpuCategory>(category));
        return bytes;
    }

    uint64_t GpuResidency::Excess(GpuCategory category) const
    {
        uint64_t excess = 0;
        const std::optional<uint64_t> &limit = mLimits[Index(category)];
        if (const uint64_t used = Used(category); limit && used > *limit)
            excess = used - *limit;
        if (const uint64_t total = UsedTotal(); total > mTotalTarget)
            excess = std::max(excess, total - mTotalTarget);
        return excess;
    }
}
