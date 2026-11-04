/**
 * @file GpuResidency.h
 * @author sumin.park
 * @brief Tracks GPU memory use and targets for freeing unused resources.
 *
 * Retiring memory is occupied until the GPU's last frame using it completes, so Used counts it. It is already
 * scheduled for release, so Excess does not ask a cache to free more on its account.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace mir
{
    enum class GpuCategory : uint8_t { Mesh, Texture, Material, Count };

    class GpuResidency
    {
    public:
        using Source = std::function<uint64_t()>;

        // Held by all categories together, active and unused. Tunable, not a hard cap.
        void SetTotalTarget(uint64_t bytes) { mTotalTarget = bytes; }
        uint64_t TotalTarget() const { return mTotalTarget; }
        void SetCategoryLimit(GpuCategory category, std::optional<uint64_t> bytes) { mLimits[Index(category)] = bytes; }

        // A cache reports the bytes it holds. The returned token ends the report.
        uint32_t AddHolder(GpuCategory category, Source held);
        void RemoveHolder(uint32_t token);
        // What the GPU still reads after release. The latest source of a category wins.
        void SetRetiring(GpuCategory category, Source retiring) { mRetiring[Index(category)] = std::move(retiring); }

        uint64_t Held(GpuCategory category) const;
        uint64_t Retiring(GpuCategory category) const;
        // What the device holds now, retiring memory included.
        uint64_t Used(GpuCategory category) const { return Held(category) + Retiring(category); }
        uint64_t UsedTotal() const;
        uint64_t HeldTotal() const;

        // What is left to free once the retiring memory is gone: held bytes over the total target, and held
        // bytes over a category's limit. A caller that frees both kinds at once counts the total only once.
        uint64_t TotalExcess() const;
        uint64_t CategoryExcess(GpuCategory category) const;
        // How many bytes a cache of this category should free, the larger of the two. Zero when under both.
        uint64_t Excess(GpuCategory category) const { return std::max(TotalExcess(), CategoryExcess(category)); }

    private:
        struct Holder
        {
            uint32_t token = 0;
            GpuCategory category = GpuCategory::Mesh;
            Source held;
        };
        static constexpr size_t Index(GpuCategory category) { return static_cast<size_t>(category); }

        uint64_t mTotalTarget = 512ull * 1024 * 1024;
        std::optional<uint64_t> mLimits[static_cast<size_t>(GpuCategory::Count)];
        Source mRetiring[static_cast<size_t>(GpuCategory::Count)];
        std::vector<Holder> mHolders;
        uint32_t mNextToken = 1;
    };
}
