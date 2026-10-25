/**
 * @file ContentBrowserWatcher.cpp
 * @author Rahul Nair
 * @brief Polls the project's content root and applies folder/file mutations.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <app/ContentBrowserWatcher.h>

#include <assets/AssetPath.h>
#include <core/log/Log.h>
#include <core/log/SetupError.h>
#include <prefab/PrefabFormat.h>

#include <system_error>

namespace mir
{
    namespace
    {
        bool IsValidEntryName(const std::string &name)
        {
            return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos &&
                name.find('\\') == std::string::npos;
        }

        uint64_t Signature(const std::filesystem::directory_entry &entry)
        {
            std::error_code error;
            const auto time = entry.last_write_time(error).time_since_epoch().count();
            const auto size = entry.file_size(error);
            return static_cast<uint64_t>(time) ^ (size + 0x9e3779b97f4a7c15ull + (static_cast<uint64_t>(time) << 6));
        }

        std::filesystem::path PrefabRoot(const GameProject &project)
        {
            const std::string prefix = project.mDir + "/";
            const std::filesystem::path assets = project.mAssetIdRoot.rfind(prefix, 0) == 0
                ? std::filesystem::path(project.mAssetIdRoot.substr(prefix.size()))
                : std::filesystem::path("assets");
            return assets / std::string(kPrefabDirectoryName);
        }

        bool IsUnder(const std::filesystem::path &path, const std::filesystem::path &root)
        {
            auto segment = path.begin();
            for (auto expected = root.begin(); expected != root.end(); ++expected, ++segment)
            {
                if (segment == path.end() || *segment != *expected)
                    return false;
            }
            return true;
        }
    } // namespace

    void ContentBrowserWatcher::Initialize(const GameProject &project)
    {
        mProject = project;
        mView = {};
        mWatched.clear();
        mWatchInitialized = false;
        mPrefabSourceChanged = false;
        mLastPoll = {};

        // LoadGameProject already found this directory, so losing it here is the machine's doing
        std::error_code error;
        if (!std::filesystem::is_directory(project.mRootDir, error))
        {
            throw SetupError(ErrorCategory::System, "The content browser cannot watch the game directory",
                project.mRootDir, error ? error.message() : "it is no longer a directory",
                "Check the directory still exists and is readable.");
        }

        mRoot = project.mRootDir;
        mPrefabRoot = PrefabRoot(project);
        mView.mContentRoot = project.mDir;
        Refresh();
    }

    void ContentBrowserWatcher::Update()
    {
        const auto now = std::chrono::steady_clock::now();
        if (mLastPoll != std::chrono::steady_clock::time_point{} && now - mLastPoll < std::chrono::milliseconds(250))
            return;
        Refresh();
    }

    void ContentBrowserWatcher::Refresh()
    {
        mLastPoll = std::chrono::steady_clock::now();
        PollWatcher();
    }

    void ContentBrowserWatcher::PollWatcher()
    {
        std::map<std::filesystem::path, WatchedFile> freshWatched;
        std::vector<ContentBrowserEntryView> entries;

        for (const auto &entry : std::filesystem::recursive_directory_iterator(mRoot))
        {
            const std::filesystem::path relative = entry.path().lexically_relative(mRoot);
            if (relative.empty())
                continue;

            ContentBrowserEntryView view;
            view.mRelativePath = relative.generic_string();
            view.mName = entry.path().filename().string();

            if (entry.is_directory())
            {
                view.mKind = ContentBrowserEntryKind::Directory;
                entries.push_back(std::move(view));
                continue;
            }
            if (!entry.is_regular_file())
                continue; // symlinks, devices: not meaningful to browse or watch

            view.mKind = ContentBrowserEntryKind::File;
            std::error_code sizeError;
            view.mSizeBytes = entry.file_size(sizeError);
            freshWatched.emplace(relative, WatchedFile{ Signature(entry) });
            entries.push_back(std::move(view));
        }

        std::vector<std::filesystem::path> added, removed;
        for (const auto &[path, file] : freshWatched)
            if (!mWatched.contains(path))
                added.push_back(path);
        for (const auto &[path, file] : mWatched)
            if (!freshWatched.contains(path))
                removed.push_back(path);

        mView.mLastRenamedFrom.clear();
        mView.mLastRenamedTo.clear();
        if (mWatchInitialized && removed.size() == 1 && added.size() == 1 &&
            mWatched.at(removed.front()).signature == freshWatched.at(added.front()).signature)
        {
            mView.mLastRenamedFrom = removed.front().generic_string();
            mView.mLastRenamedTo = added.front().generic_string();
        }

        if (mWatchInitialized)
        {
            for (const auto &[path, file] : freshWatched)
            {
                if (path.extension() != kPrefabExtension || !IsUnder(path, mPrefabRoot))
                    continue;
                const auto previous = mWatched.find(path);
                if (previous == mWatched.end() || previous->second.signature != file.signature)
                    mPrefabSourceChanged = true;
            }
            for (const auto &[path, file] : mWatched)
            {
                (void)file;
                if (path.extension() == kPrefabExtension && IsUnder(path, mPrefabRoot) &&
                    !freshWatched.contains(path))
                {
                    mPrefabSourceChanged = true;
                }
            }
        }

        mWatched = std::move(freshWatched);
        mWatchInitialized = true;
        mView.mEntries = std::move(entries);
    }

    void ContentBrowserWatcher::Handle(const ContentBrowserAction &action)
    {
        if (action.mKind == ContentBrowserActionKind::None)
            return;
        mView.mLastError.clear();
        switch (action.mKind)
        {
        case ContentBrowserActionKind::CreateFolder:
            CreateFolder(action.mTargetPath, action.mNewName);
            break;
        case ContentBrowserActionKind::Rename:
            RenameEntry(action.mTargetPath, action.mNewName);
            break;
        case ContentBrowserActionKind::Delete:
            DeleteEntry(action.mTargetPath);
            break;
        case ContentBrowserActionKind::OpenInIde:
        case ContentBrowserActionKind::OpenPrefab:
            break;
        case ContentBrowserActionKind::None:
            break;
        }
        Refresh();
    }

    void ContentBrowserWatcher::Reject(const std::string &path, std::string diagnostic)
    {
        mView.mLastError = path + ": " + diagnostic;
    }

    void ContentBrowserWatcher::CreateFolder(const std::string &parentRelative, const std::string &name)
    {
        if (!IsValidEntryName(name))
        {
            mView.mLastError = "'" + name + "' is not a valid folder name";
            return;
        }
        const std::filesystem::path target = mRoot / parentRelative / name;
        std::error_code error;
        if (std::filesystem::exists(target, error))
        {
            mView.mLastError = name + " already exists";
            return;
        }
        if (!std::filesystem::create_directories(target, error))
        {
            mView.mLastError = "could not create '" + name + "': " + error.message();
            MIR_LOG_ERROR("Content browser: could not create '{}': {}", target.string(), error.message());
        }
    }

    void ContentBrowserWatcher::RenameEntry(const std::string &targetRelative, const std::string &newName)
    {
        if (!IsValidEntryName(newName))
        {
            mView.mLastError = "'" + newName + "' is not a valid name";
            return;
        }
        const std::filesystem::path source = mRoot / targetRelative;
        const std::filesystem::path destination = source.parent_path() / newName;
        std::error_code error;
        if (std::filesystem::exists(destination, error))
        {
            mView.mLastError = newName + " already exists";
            return;
        }
        std::filesystem::rename(source, destination, error);
        if (error)
        {
            mView.mLastError = "could not rename '" + targetRelative + "': " + error.message();
            MIR_LOG_ERROR("Content browser: could not rename '{}': {}", source.string(), error.message());
        }
    }

    void ContentBrowserWatcher::DeleteEntry(const std::string &targetRelative)
    {
        const std::filesystem::path target = mRoot / targetRelative;
        std::error_code error;
        std::filesystem::remove_all(target, error);
        if (error)
        {
            mView.mLastError = "could not delete '" + targetRelative + "': " + error.message();
            MIR_LOG_ERROR("Content browser: could not delete '{}': {}", target.string(), error.message());
        }
    }

    std::optional<std::string> ContentBrowserWatcher::AssetLogicalPath(const std::string &relativePath) const
    {
        const std::string dirPrefix = mProject.mDir + "/";
        if (mProject.mAssetIdRoot.size() <= dirPrefix.size() ||
            mProject.mAssetIdRoot.compare(0, dirPrefix.size(), dirPrefix) != 0)
            return std::nullopt;
        const std::string assetsPrefix = mProject.mAssetIdRoot.substr(dirPrefix.size()) + "/";
        if (relativePath.size() <= assetsPrefix.size() ||
            relativePath.compare(0, assetsPrefix.size(), assetsPrefix) != 0)
            return std::nullopt;

        const AssetPathResult logical =
            MakeLogicalAssetPath(mProject.mAssetIdRoot, relativePath.substr(assetsPrefix.size()));
        return logical.Ok() ? std::optional<std::string>(logical.path) : std::nullopt;
    }
} // namespace mir
