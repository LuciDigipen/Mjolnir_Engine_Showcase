/**
 * @file EditorExtension.cpp
 * @author Jackey Cen
 * @brief The hooks an outside module registers with to add panels, viewport UI and an owned edit mode.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/EditorExtension.h>

#include <algorithm>

namespace mir
{
    Registration EditorExtensions::Add(IEditorExtension &extension)
    {
        const uint64_t id = mNextId++;
        mEntries.push_back(Entry{ id, &extension });
        return Registration(mOwner, [this, id] { Remove(id); });
    }

    void EditorExtensions::Remove(uint64_t id)
    {
        const auto entry = std::find_if(mEntries.begin(), mEntries.end(),
            [id](const Entry &candidate) { return candidate.mId == id; });
        if (entry != mEntries.end())
            mEntries.erase(entry);
    }

    void EditorExtensions::Update(const EditorExtensionContext &context) const
    {
        for (const Entry &entry : mEntries)
            entry.mExtension->Update(context);
    }

    void EditorExtensions::DrawWindows(const EditorExtensionContext &context) const
    {
        for (const Entry &entry : mEntries)
            entry.mExtension->DrawWindows(context);
    }

    void EditorExtensions::DrawToolbar(const EditorExtensionContext &context) const
    {
        for (const Entry &entry : mEntries)
            entry.mExtension->DrawToolbar(context);
    }

    void EditorExtensions::DrawOverlay(const EditorExtensionContext &context) const
    {
        for (const Entry &entry : mEntries)
            entry.mExtension->DrawOverlay(context);
    }

    bool EditorExtensions::OwnsViewport() const
    {
        return std::any_of(mEntries.begin(), mEntries.end(),
            [](const Entry &entry) { return entry.mExtension->OwnsViewport(); });
    }

    std::vector<const char *> EditorExtensions::WindowNames() const
    {
        std::vector<const char *> names;
        for (const Entry &entry : mEntries)
        {
            const std::span<const char *const> windows = entry.mExtension->WindowNames();
            names.insert(names.end(), windows.begin(), windows.end());
        }
        return names;
    }
} // namespace mir
