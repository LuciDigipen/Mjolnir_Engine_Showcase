/**
 * @file ContentBrowserPanel.h
 * @author Rahul Nair
 * @brief Content browser panel: a folder tree and file listing for the open project's content root.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mir
{
    // A Prefab row's content path, as a drag payload for the Hierarchy and the scene viewport.
    inline constexpr const char *kPrefabContentPayload = "MIR_PREFAB_CONTENT";

    enum class ContentBrowserEntryKind
    {
        File,
        Directory,
    };

    struct ContentBrowserEntryView
    {
        std::string mRelativePath; // generic-slash, relative to the content root
        std::string mName;
        ContentBrowserEntryKind mKind = ContentBrowserEntryKind::File;
        uint64_t mSizeBytes = 0; // 0 for directories
        bool mUnsaved = false;   // a memory-only Prefab at its intended path
    };

    struct ContentBrowserView
    {
        std::string mContentRoot; // display only, e.g. "games/HelloWorld"
        std::string mLastError;
        std::vector<ContentBrowserEntryView> mEntries; // every file/dir under the content root, flat

        std::string mLastRenamedFrom;
        std::string mLastRenamedTo;
    };

    enum class ContentBrowserActionKind
    {
        None,
        CreateFolder,
        Rename,
        Delete,
        OpenInIde,    // double-clicked a script file - mTargetPath is the file
        OpenPrefab,   // double-clicked a Prefab - mTargetPath is the file
    };

    struct ContentBrowserAction
    {
        ContentBrowserActionKind mKind = ContentBrowserActionKind::None;
        std::string mTargetPath;
        std::string mNewName;
    };

    class ContentBrowserPanel
    {
    public:
        ContentBrowserAction Draw(const ContentBrowserView &view);

    private:
        void DrawEntries(const ContentBrowserView &view, ContentBrowserAction &action);
        void DrawDeleteConfirm(ContentBrowserAction &action);

        std::string mCurrentFolder;
        std::string mSelectedPath;
        std::string mRenameTarget;
        char mRenameBuffer[256]{};
        std::string mPendingDelete;
        bool mPendingDeleteIsFolder = false;
        bool mDeleteConfirmRequested = false;
        bool mCreatingFolder = false;
        char mNewFolderBuffer[128]{};
    };
} // namespace mir
