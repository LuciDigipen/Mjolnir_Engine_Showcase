/**
 * @file EditorExtension.cpp
 * @author Jackey Cen
 * @brief The hooks an outside module registers with to add panels, viewport UI and an owned edit mode.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/EditorExtension.h>

#include <editor/ContentBrowserPaths.h>
#include <editor/history/History.h>

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

    void EditorExtensions::FlushHistory(History &history, World &world, ISceneOps &scene) const
    {
        if (history.HasPendingWork())
            for (const Entry &entry : mEntries)
                entry.mExtension->BeforeHistoryFlush(history);
        history.Flush(world, scene);
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

    std::vector<std::string> EditorExtensions::ContentBrowserCreateItems() const
    {
        std::vector<std::string> items;
        for (const Entry &entry : mEntries)
        {
            for (const char *const item : entry.mExtension->ContentBrowserCreateItems())
                if (item != nullptr)
                    items.emplace_back(item);
        }
        return items;
    }

    void EditorExtensions::CreateContentBrowserItem(std::string_view item, std::string_view assetsRoot,
        std::string_view folder) const
    {
        const std::optional<std::string> assetsFolder = AssetsRelativeFolder(assetsRoot, folder);
        for (const Entry &entry : mEntries)
        {
            for (const char *const candidate : entry.mExtension->ContentBrowserCreateItems())
            {
                if (candidate == nullptr || item != candidate)
                    continue;
                entry.mExtension->OnContentBrowserCreateItem(candidate, assetsFolder
                    ? std::optional<std::string_view>(*assetsFolder) : std::nullopt);
                return;
            }
        }
    }

    bool EditorExtensions::OpenContentBrowserFile(std::string_view assetsRoot,
        std::string_view browserRelativePath) const
    {
        const std::optional<std::string> assetsRelative = AssetsRelativeFolder(assetsRoot, browserRelativePath);
        if (!assetsRelative)
            return false;
        for (const Entry &entry : mEntries)
            if (entry.mExtension->OpenContentBrowserFile(*assetsRelative))
                return true;
        return false;
    }

    std::vector<std::string> EditorExtensions::UnsavedDraftNames() const
    {
        std::vector<std::string> names;
        for (const Entry &entry : mEntries)
        {
            for (const char *const name : entry.mExtension->UnsavedDraftNames())
                if (name != nullptr && name[0] != '\0')
                    names.emplace_back(name);
        }
        return names;
    }
} // namespace mir
