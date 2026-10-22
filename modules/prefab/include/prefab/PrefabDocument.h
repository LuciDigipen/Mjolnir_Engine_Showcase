/**
 * @file PrefabDocument.h
 * @author Jackey Cen
 * @brief In-memory .prefab document: nodes, patches and the node-id
 *        high-water mark.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "prefab/PrefabId.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mir
{
    using PrefabNodeId = uint64_t;
    inline constexpr PrefabNodeId kNullPrefabNodeId = 0;

    // Nested occurrence ids, then the final entity node id.
    using PrefabNodeAddress = std::vector<PrefabNodeId>;

    enum class PrefabNodeKind : uint8_t
    {
        Entity,
        Prefab,
    };

    struct PrefabComponentInstance
    {
        std::string mType;
        nlohmann::json mFields = nlohmann::json::object();
    };

    struct PrefabFieldOverride
    {
        std::string mComponent;
        std::string mField;
        nlohmann::json mValue;
    };

    struct PrefabPatch
    {
        PrefabNodeAddress mTarget;
        std::vector<PrefabFieldOverride> mFields;
        std::vector<PrefabComponentInstance> mAddedComponents;
        std::vector<std::string> mRemovedComponents;
    };

    struct PrefabNode
    {
        PrefabNodeId mId = kNullPrefabNodeId;
        PrefabNodeKind mKind = PrefabNodeKind::Entity;
        PrefabNodeId mParent = kNullPrefabNodeId;

        // entity nodes only
        std::vector<PrefabComponentInstance> mComponents;

        // nested-reference nodes only
        PrefabId mPrefab{};
        std::vector<PrefabPatch> mPatches;
    };

    struct PrefabDocument
    {
        PrefabId mId{};

        // high-water mark: ids below it are never reissued
        PrefabNodeId mNextNodeId = 1;
        PrefabNodeId mRootNode = kNullPrefabNodeId;
        std::vector<PrefabNode> mNodes;
    };

    PrefabDocument NewPrefabDocument();

    // Empty at the top of the id space rather than wrapping onto a used id.
    std::optional<PrefabNodeId> AllocateNodeId(PrefabDocument &doc);
}
