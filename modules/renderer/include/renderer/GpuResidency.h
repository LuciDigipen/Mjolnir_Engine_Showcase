/**
 * @file GpuResidency.h
 * @author sumin.park
 * @brief One renderer-wide GPU residency target with optional category limits.
 *
 * Caches report what they hold and ask how much they must free. Active claims are the caches' business,
 * this object only counts bytes. Allocations the GPU still uses count until they are released.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace mir
{
    enum class GpuCategory : uint8_t { Mesh, Texture, Count };

    class GpuResidency
    {
    public:
        using Source = std::function<uint64_t()>;

        // Held by all categories together, active, unused and retiring. Tunable, not a hard cap.
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
        uint64_t Used(GpuCategory category) const { return Held(category) + Retiring(category); }
        uint64_t UsedTotal() const;

        // How many bytes a cache of this category should free, the larger of its category overage and the
        // total overage. Zero when under both.
        uint64_t Excess(GpuCategory category) const;

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
