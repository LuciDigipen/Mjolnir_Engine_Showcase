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
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    struct EditorPreferences;
    struct ContentBrowserPreview
    {
        uint64_t mImage = 0;
        uint32_t mWidth = 0, mHeight = 0;
    };
    // A Prefab row's content path, as a drag payload for the Hierarchy and the scene viewport.
    inline constexpr const char *kPrefabContentPayload = "MIR_PREFAB_CONTENT";
    inline constexpr const char *kParticleContentPayload = "MIR_PARTICLE_CONTENT";

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
        std::function<ContentBrowserPreview(const std::string &)> mPreview;
        std::string mContentRoot; // display only, e.g. "games/HelloWorld"
        std::string mLastError;
        std::vector<ContentBrowserEntryView> mEntries; // every file/dir under the content root, flat
        std::vector<std::string> mExtensionCreateItems; // extension buttons beside New Particle System

        std::string mLastRenamedFrom;
        std::string mLastRenamedTo;
    };

    enum class ContentBrowserActionKind
    {
        None,
        CreateFolder,
        Rename,
        Delete,
        OpenFile,     // double-clicked a file - mTargetPath is the file
        NewParticleSystem,
        NewExtensionItem, // mItemLabel names the clicked button, mTargetPath is the folder
    };

    struct ContentBrowserAction
    {
        bool mPreferencesChanged = false;
        ContentBrowserActionKind mKind = ContentBrowserActionKind::None;
        std::string mTargetPath;
        std::string mNewName;
        std::string mItemLabel;
    };

    enum class ContentBrowserOpenTarget
    {
        None,
        ParticleSystem,
        Prefab,
        Ide,
    };

    // What a double-clicked file opens when no extension takes it.
    ContentBrowserOpenTarget ClassifyContentBrowserFile(std::string_view relativePath);

    class ContentBrowserPanel
    {
    public:
        ContentBrowserAction Draw(const ContentBrowserView &view, EditorPreferences &preferences);
        const std::string &SelectedPath() const { return mSelectedPath; }

    private:
        void DrawEntries(const ContentBrowserView &view, ContentBrowserAction &action, EditorPreferences &preferences);
        void DrawEntry(const ContentBrowserEntryView &entry, ContentBrowserAction &action, bool grid,
            float size, const ContentBrowserView &view);
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
