/**
 * @file PrefabPlayMarks.cpp
 * @author Jackey Cen
 * @brief Keeps the unsaved marks of Prefab documents across Play and Stop.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <app/PrefabPlayMarks.h>

namespace mir
{
    PrefabPlayMarks CapturePrefabPlayMarks(std::span<const DocumentId> documents,
        const DocumentUnsavedQuery &isUnsaved)
    {
        PrefabPlayMarks marks;
        for (const DocumentId document : documents)
        {
            if (isUnsaved(document))
                marks.mUnsaved.push_back(document);
        }
        return marks;
    }

    void RestorePrefabPlayMarks(const PrefabPlayMarks &marks, const DocumentUnsavedSetter &setUnsaved)
    {
        for (const DocumentId document : marks.mUnsaved)
            setUnsaved(document);
    }
}
