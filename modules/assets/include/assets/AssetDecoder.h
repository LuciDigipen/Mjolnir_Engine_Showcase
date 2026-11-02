/**
 * @file AssetDecoder.h
 * @author sumin.park
 * @brief How a cooked blob becomes a typed object, and the blobs it may read on the way.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "assets/AssetBlob.h"
#include "assets/AssetId.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace mir
{
    // The other blobs a decoder can read
    class AssetDependencies
    {
    public:
        using Lookup = std::function<const AssetBlobView *(AssetId)>;

        explicit AssetDependencies(Lookup lookup) : mLookup(std::move(lookup)) {}

        // The blob with this id, or null when it was not loaded. The view is valid only during the decode.
        const AssetBlobView *Find(AssetId id) const { return mLookup(id); }

    private:
        Lookup mLookup;
    };

    // Decodes root into out, or returns false with error set.
    template <class T>
    using AssetDecodeFunction = bool (*)(const AssetBlobView &root, const AssetDependencies &dependencies, T &out,
        std::string &error);

    // A decode function and the dependencies it reads. A descriptor is identified by its address,
    // so it needs static storage.
    template <class T>
    struct AssetDecoder
    {
        AssetDecodeFunction<T> decode = nullptr;
        // Type tags of the dependencies to load. Empty means the root alone.
        std::span<const uint32_t> dependencyTags;
        // Owned decoded bytes, including dynamic allocations. Supply for variable-sized results.
        uint64_t (*sizeBytes)(const T &) = nullptr;
    };
}
