/**
 * @file PrefabPlayMarks.h
 * @author Jackey Cen
 * @brief Keeps the unsaved marks of Prefab documents across Play and Stop.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/history/DocumentId.h>

#include <functional>
#include <span>
#include <vector>

namespace mir
{
    struct PrefabPlayMarks
    {
        std::vector<DocumentId> mUnsaved;
    };

    using DocumentUnsavedQuery = std::function<bool(DocumentId)>;
    using DocumentUnsavedSetter = std::function<void(DocumentId)>;

    PrefabPlayMarks CapturePrefabPlayMarks(std::span<const DocumentId> documents,
        const DocumentUnsavedQuery &isUnsaved);
    void RestorePrefabPlayMarks(const PrefabPlayMarks &marks, const DocumentUnsavedSetter &setUnsaved);
}
