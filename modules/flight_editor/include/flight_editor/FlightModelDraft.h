/**
 * @file FlightModelDraft.h
 * @author Jackey Cen
 * @brief One open *.aircraft.json, its unsaved edits and its reload and save rules.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <flight/AircraftConfig.h>
#include <flight_debug/FlightDebugSaving.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace mir
{
    enum class FlightModelSaveResult
    {
        Saved,
        Invalid,
        ChangedOnDisk,
    };

    enum class FlightModelOpenAction
    {
        Open,
        Ask,
    };

    // A dirty draft needs Save, Discard or Cancel before another file replaces it.
    FlightModelOpenAction EvaluateFlightModelOpen(bool dirty, std::string_view openPath,
        std::string_view requestedPath);

    class FlightModelDraft
    {
    public:
        // A file that does not parse keeps its path and parser message, with no config.
        bool Open(const std::filesystem::path &sourceAssetsRoot, std::string_view assetsRelativePath,
            std::string &error);
        bool Loaded() const { return mLoaded; }
        bool HasFile() const { return !mPath.empty(); }
        const std::string &Path() const { return mPath; }
        const std::string &ParseError() const { return mParseError; }
        const AircraftConfig *Config() const { return mLoaded ? &mConfig : nullptr; }
        AircraftConfig *Config() { return mLoaded ? &mConfig : nullptr; }

        bool Dirty() const { return mDirty; }
        void MarkEdited() { mDirty = mLoaded; }

        // True when the file's stamp differs from the one taken at open or the last save.
        bool DiskChanged(const std::filesystem::path &sourceAssetsRoot) const;

        // True when a clean draft reloaded a file that changed on disk.
        bool Poll(const std::filesystem::path &sourceAssetsRoot);

        // Reloads the file, dropping unsaved edits even when it no longer parses.
        bool Revert(const std::filesystem::path &sourceAssetsRoot, std::string &error);

        // ChangedOnDisk when the file changed and overwrite is false; nothing is written then.
        FlightModelSaveResult Save(const std::filesystem::path &sourceAssetsRoot,
            const SavedSourceCallback &onSaved, std::string &error, bool overwrite = false);

    private:
        struct FileStamp
        {
            std::uintmax_t size = 0;
            std::filesystem::file_time_type time{};

            bool operator==(const FileStamp &other) const
            {
                return size == other.size && time == other.time;
            }
        };

        bool Read(const std::filesystem::path &sourceAssetsRoot, std::string &error);
        std::filesystem::path Absolute(const std::filesystem::path &sourceAssetsRoot) const;
        static std::optional<FileStamp> Stamp(const std::filesystem::path &path);

        std::string mPath;
        std::string mParseError;
        AircraftConfig mConfig;
        std::optional<FileStamp> mStamp;
        bool mLoaded = false;
        bool mDirty = false;
    };
} // namespace mir
