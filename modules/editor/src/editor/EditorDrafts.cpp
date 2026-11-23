/**
 * @file EditorDrafts.cpp
 * @author Jackey Cen
 * @brief Names all unsaved drafts the editor would discard, for its prompts.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/EditorDrafts.h>

#include <cstddef>

namespace mir
{
    std::string DraftListPhrase(std::span<const std::string> names)
    {
        std::string phrase;
        for (std::size_t index = 0; index < names.size(); ++index)
        {
            if (index != 0)
                phrase += index + 1 == names.size() ? " and " : ", ";
            phrase += names[index];
        }
        return phrase;
    }

    std::string DiscardDraftsWarning(std::span<const std::string> names)
    {
        if (names.empty())
            return {};
        return "Unsaved " + DraftListPhrase(names) + " will be discarded, never saved automatically.";
    }
} // namespace mir
