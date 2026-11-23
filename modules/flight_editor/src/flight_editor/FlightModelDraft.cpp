/**
 * @file FlightModelDraft.cpp
 * @author Jackey Cen
 * @brief One open *.aircraft.json, its unsaved edits and its reload and save rules.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight_editor/FlightModelDraft.h>

#include <core/fs/FileIO.h>

#include <format>
#include <system_error>

namespace mir
{
    FlightModelOpenAction EvaluateFlightModelOpen(bool dirty, std::string_view openPath,
        std::string_view requestedPath)
    {
        return dirty && openPath != requestedPath ? FlightModelOpenAction::Ask : FlightModelOpenAction::Open;
    }

    std::optional<FlightModelDraft::FileStamp> FlightModelDraft::Stamp(const std::filesystem::path &path)
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error))
            return std::nullopt;

        const std::uintmax_t size = std::filesystem::file_size(path, error);
        if (error)
            return std::nullopt;
        const std::filesystem::file_time_type time = std::filesystem::last_write_time(path, error);
        if (error)
            return std::nullopt;
        return FileStamp{ size, time };
    }

    std::filesystem::path FlightModelDraft::Absolute(const std::filesystem::path &sourceAssetsRoot) const
    {
        return sourceAssetsRoot / std::filesystem::path(mPath);
    }

    bool FlightModelDraft::Open(const std::filesystem::path &sourceAssetsRoot, std::string_view assetsRelativePath,
        std::string &error)
    {
        mPath = assetsRelativePath;
        mDirty = false;
        return Read(sourceAssetsRoot, error);
    }

    bool FlightModelDraft::Read(const std::filesystem::path &sourceAssetsRoot, std::string &error)
    {
        mConfig = {};
        mLoaded = false;
        mParseError.clear();
        mStamp = Stamp(Absolute(sourceAssetsRoot));

        const std::optional<std::string> text = ReadText(Absolute(sourceAssetsRoot));
        if (!text)
        {
            mParseError = std::format("Could not read '{}'.", mPath);
            error = mParseError;
            return false;
        }

        AircraftConfig parsed;
        std::string invalid;
        if (!ParseAircraftConfig(*text, parsed, invalid))
        {
            mParseError = invalid;
            error = invalid;
            return false;
        }

        mConfig = std::move(parsed);
        mLoaded = true;
        error.clear();
        return true;
    }

    bool FlightModelDraft::DiskChanged(const std::filesystem::path &sourceAssetsRoot) const
    {
        return Stamp(Absolute(sourceAssetsRoot)) != mStamp;
    }

    bool FlightModelDraft::Poll(const std::filesystem::path &sourceAssetsRoot)
    {
        if (!HasFile() || mDirty || !DiskChanged(sourceAssetsRoot))
            return false;

        std::string error;
        Read(sourceAssetsRoot, error);
        return true;
    }

    bool FlightModelDraft::Revert(const std::filesystem::path &sourceAssetsRoot, std::string &error)
    {
        mDirty = false;
        return Read(sourceAssetsRoot, error);
    }

    FlightModelSaveResult FlightModelDraft::Save(const std::filesystem::path &sourceAssetsRoot,
        const SavedSourceCallback &onSaved, std::string &error, bool overwrite)
    {
        if (!mLoaded)
        {
            error = mParseError.empty() ? "The config is not loaded." : mParseError;
            return FlightModelSaveResult::Invalid;
        }
        if (!overwrite && DiskChanged(sourceAssetsRoot))
        {
            error = std::format("'{}' changed on disk since it was opened or saved.", mPath);
            return FlightModelSaveResult::ChangedOnDisk;
        }
        if (!SaveAircraftConfigToSource(mConfig, sourceAssetsRoot, mPath, error, onSaved))
            return FlightModelSaveResult::Invalid;

        mStamp = Stamp(Absolute(sourceAssetsRoot));
        mDirty = false;
        return FlightModelSaveResult::Saved;
    }
} // namespace mir
