/**
 * @file DocumentId.h
 * @author Dong Ta
 * @brief Which authoring document a step changes: the open scene, or a document App names, such as a Prefab.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <compare>
#include <cstdint>

namespace mir
{
    //128 bits, so a Prefab's UUID can be used as is; history never reads the meaning
    struct DocumentId
    {
        uint64_t mHigh = 0;
        uint64_t mLow = 0;

        constexpr bool operator==(const DocumentId &) const = default;
        constexpr auto operator<=>(const DocumentId &) const = default;
    };

    //the null UUID, which no Prefab can have
    inline constexpr DocumentId kSceneDocument{};
}
