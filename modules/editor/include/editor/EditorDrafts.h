/**
 * @file EditorDrafts.h
 * @author Jackey Cen
 * @brief Names all unsaved drafts the editor would discard, for its prompts.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <span>
#include <string>

namespace mir
{
    // "a", "a and b", "a, b and c"; empty when there is nothing to name.
    std::string DraftListPhrase(std::span<const std::string> names);

    // The line the project dialogs show; empty when there is nothing unsaved.
    std::string DiscardDraftsWarning(std::span<const std::string> names);
} // namespace mir
