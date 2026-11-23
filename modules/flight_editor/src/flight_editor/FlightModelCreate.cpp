/**
 * @file FlightModelCreate.cpp
 * @author Jackey Cen
 * @brief Validates a new flight model settings path and writes the chosen template.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight_editor/FlightModelCreate.h>

#include <flight/AircraftConfig.h>
#include <terrain/TerrainAssetPath.h>

#include <core/ecs/ComponentFields.h>
#include <core/fs/FileIO.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <system_error>

namespace mir
{
    namespace
    {
        constexpr std::string_view kSuffix = ".aircraft.json";
        constexpr std::string_view kDefaultFolder = "aircraft";

        bool IsNameCharacter(char value)
        {
            const unsigned char character = static_cast<unsigned char>(value);
            return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') || character == '_' || character == '-';
        }

        char Lowered(char value)
        {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
        }

        bool EqualsIgnoringCase(std::string_view left, std::string_view right)
        {
            return left.size() == right.size() &&
                   std::equal(left.begin(), left.end(), right.begin(),
                       [](char a, char b) { return Lowered(a) == Lowered(b); });
        }

        bool IsReservedName(std::string_view name)
        {
            static constexpr std::string_view kNames[] = { "con", "prn", "aux", "nul", "com1", "com2", "com3",
                "com4", "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6",
                "lpt7", "lpt8", "lpt9" };
            return std::ranges::any_of(kNames, [name](std::string_view reserved)
                { return EqualsIgnoringCase(name, reserved); });
        }

        bool ExistsIgnoringCase(const std::filesystem::path &directory, std::string_view fileName)
        {
            std::error_code error;
            if (!std::filesystem::is_directory(directory, error))
                return false;

            for (std::filesystem::directory_iterator entry(directory, error), end; !error && entry != end;
                entry.increment(error))
            {
                if (EqualsIgnoringCase(entry->path().filename().string(), fileName))
                    return true;
            }
            return false;
        }

        std::string FolderOf(const FlightModelCreateRequest &request)
        {
            return request.assetsFolder ? *request.assetsFolder : std::string(kDefaultFolder);
        }
    } // namespace

    FlightModelCreatePreview PreviewFlightModelCreate(const FlightModelCreateRequest &request,
        const std::filesystem::path &sourceAssetsRoot)
    {
        FlightModelCreatePreview preview;
        const std::string folder = FolderOf(request);
        const std::string fileName = request.name + std::string(kSuffix);
        preview.assetsRelativePath =
            folder.empty() ? fileName : folder + '/' + fileName;

        if (request.name.empty())
        {
            preview.error = "Enter a name.";
            return preview;
        }
        if (!std::ranges::all_of(request.name, IsNameCharacter))
        {
            preview.error = "Use only letters, digits, '_' and '-'.";
            return preview;
        }
        if (IsReservedName(request.name))
        {
            preview.error = std::format("'{}' is a reserved Windows name.", request.name);
            return preview;
        }
        if (sourceAssetsRoot.empty())
        {
            preview.error = "The project has no source assets root.";
            return preview;
        }
        if (IsTerrainFileExcludedFromCook(preview.assetsRelativePath))
        {
            preview.error = std::format("The asset cook skips '{}', so choose another folder.",
                preview.assetsRelativePath);
            return preview;
        }
        if (ExistsIgnoringCase(sourceAssetsRoot / std::filesystem::path(folder), fileName))
        {
            preview.error = std::format("'{}' already exists.", preview.assetsRelativePath);
            return preview;
        }
        if (preview.assetsRelativePath.size() >= kFieldStringCapacity)
        {
            preview.error = std::format("'{}' is longer than the config field holds.",
                preview.assetsRelativePath);
            return preview;
        }
        return preview;
    }

    bool CreateFlightModelFile(const FlightModelCreateRequest &request,
        const std::filesystem::path &sourceAssetsRoot, std::string &assetsRelativePath,
        std::filesystem::path &absolutePath, std::string &error)
    {
        const FlightModelCreatePreview preview = PreviewFlightModelCreate(request, sourceAssetsRoot);
        if (!preview.Valid())
        {
            error = preview.error;
            return false;
        }

        const std::filesystem::path target = sourceAssetsRoot / std::filesystem::path(preview.assetsRelativePath);
        std::error_code directoryError;
        if (target.has_parent_path())
            std::filesystem::create_directories(target.parent_path(), directoryError);
        if (directoryError)
        {
            error = std::format("Could not create '{}': {}", target.parent_path().string(),
                directoryError.message());
            return false;
        }

        AircraftConfig templateConfig;
        std::string parseError;
        if (!ParseAircraftConfig(FlightModelTemplateJson(request.templateKind), templateConfig, parseError))
        {
            error = std::format("The built-in template does not parse: {}", parseError);
            return false;
        }

        if (!WriteTextAtomic(target, SerializeAircraftConfig(templateConfig)))
        {
            error = std::format("Could not write '{}'.", target.string());
            return false;
        }

        assetsRelativePath = preview.assetsRelativePath;
        absolutePath = target;
        error.clear();
        return true;
    }
} // namespace mir
