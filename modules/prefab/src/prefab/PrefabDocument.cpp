/**
 * @file PrefabDocument.cpp
 * @author Jackey Cen
 * @brief In-memory .prefab document: nodes, patches and the node-id
 *        high-water mark.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "prefab/PrefabDocument.h"

#include <limits>

namespace mir
{
    PrefabDocument NewPrefabDocument()
    {
        PrefabDocument doc;
        doc.mId = GeneratePrefabId();

        PrefabNode root;
        root.mId = 1;
        root.mKind = PrefabNodeKind::Entity;
        doc.mNodes.push_back(std::move(root));

        doc.mRootNode = 1;
        doc.mNextNodeId = 2;
        return doc;
    }

    std::optional<PrefabNodeId> AllocateNodeId(PrefabDocument &doc)
    {
        if (doc.mNextNodeId == std::numeric_limits<PrefabNodeId>::max())
            return std::nullopt;
        return doc.mNextNodeId++;
    }
}
