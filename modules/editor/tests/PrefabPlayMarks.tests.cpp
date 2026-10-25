/**
 * @file PrefabPlayMarks.tests.cpp
 * @author Jackey Cen
 * @brief Tests for editor/PrefabPlayMarks.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/PrefabPlayMarks.h>

#include <editor/history/History.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace
{
    using mir::CapturePrefabPlayMarks;
    using mir::DocumentId;
    using mir::History;
    using mir::PrefabPlayMarks;
    using mir::RestorePrefabPlayMarks;
}

TEST_CASE("Prefab document unsaved marks come back after a History clear", "[app][authoring]")
{
    History history;
    const DocumentId first{ 1, 2 };
    const DocumentId second{ 3, 4 };
    const DocumentId saved{ 5, 6 };

    history.SetUnsaved(first, true);
    history.SetUnsaved(second, true);
    history.SetUnsaved(saved, false);

    const std::vector<DocumentId> documents{ first, saved };
    const PrefabPlayMarks marks = CapturePrefabPlayMarks(documents,
        [&history](DocumentId document) { return history.IsUnsaved(document); });
    history.Clear();
    history.SetUnsaved(first, false);
    history.SetUnsaved(second, false);
    RestorePrefabPlayMarks(marks,
        [&history](DocumentId document) { history.SetUnsaved(document, true); });

    CHECK(history.IsUnsaved(first));
    CHECK_FALSE(history.IsUnsaved(second));
    CHECK_FALSE(history.IsUnsaved(saved));
}
