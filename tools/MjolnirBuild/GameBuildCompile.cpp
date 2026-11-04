/**
 * @file GameBuildCompile.cpp
 * @author Quy Dong Ta
 * @brief The game build step: writes builds/<preset>/mjolnir_game_build.bat, runs it with its output in
 *        mjolnir_game_build.log, and swaps the fresh install into the template's place.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "GameBuildCompile.h"

#include "BuildOutput.h"

#include <core/fs/FileIO.h>
#include <core/fs/Paths.h>
#include <core/platform/Process.h>
#include <project/GameBuildInfo.h>
#include <serial/JsonFile.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace mir::build
{
    namespace fs = std::filesystem;
    using Json = nlohmann::json;

    namespace
    {
        // Only what the template installs, not the tests and tools the preset also builds
        constexpr std::string_view kGameBuildTargets = "MjolnirEngine MjolnirErrorReporter";
        constexpr std::string_view kScriptFileName = "mjolnir_game_build.bat";
        constexpr std::string_view kLogFileName = "mjolnir_game_build.log";
        // In an unfinished install, so the next compile knows the folder is its own to remove
        constexpr std::string_view kStagingMarkerFileName = "mjolnir_game_build_install";

        std::string_view Trim(std::string_view text)
        {
            const std::size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos)
                return {};
            return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        }

        std::string Upper(std::string_view text)
        {
            std::string upper(text);
            std::ranges::transform(upper, upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return upper;
        }

        // CMake's spellings of a boolean
        std::optional<bool> CMakeBool(std::string_view value)
        {
            const std::string upper = Upper(Trim(value));
            for (std::string_view on : { "ON", "TRUE", "YES", "Y", "1" })
            {
                if (upper == on)
                    return true;
            }
            for (std::string_view off : { "OFF", "FALSE", "NO", "N", "0", "" })
            {
                if (upper == off)
                    return false;
            }
            return std::nullopt;
        }

        // NAME:TYPE=VALUE in CMakeCache.txt
        std::optional<std::string_view> CacheValue(std::string_view cache, std::string_view name)
        {
            while (!cache.empty())
            {
                const std::size_t end = cache.find('\n');
                const std::string_view line = Trim(cache.substr(0, end));
                cache = end == std::string_view::npos ? std::string_view() : cache.substr(end + 1);

                if (line.size() > name.size() && line.starts_with(name) &&
                    (line[name.size()] == ':' || line[name.size()] == '='))
                {
                    const std::size_t equals = line.find('=');
                    if (equals != std::string_view::npos)
                        return line.substr(equals + 1);
                }
            }
            return std::nullopt;
        }

        fs::path FromText(std::string_view text)
        {
            return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.data()), text.size()));
        }

        // Backslashed, with % doubled so the batch file reads it as written
        std::string BatchPath(const fs::path &path)
        {
            std::string text;
            for (const char c : PathText(fs::path(path).make_preferred()))
                text += c == '%' ? std::string("%%") : std::string(1, c);
            return text;
        }

        bool IsOwnStaging(const fs::path &staging)
        {
            std::error_code ec;
            return fs::is_regular_file(staging / kStagingMarkerFileName, ec);
        }

        std::optional<fs::path> EnvironmentPath(const wchar_t *name)
        {
            wchar_t *value = nullptr;
            std::size_t length = 0;
            if (_wdupenv_s(&value, &length, name) != 0 || value == nullptr)
                return std::nullopt;
            const fs::path path(value);
            std::free(value);
            return path.empty() ? std::nullopt : std::optional(path);
        }

        fs::path CommandShell()
        {
            if (const std::optional<fs::path> comspec = EnvironmentPath(L"ComSpec"))
                return *comspec;
            return EnvironmentPath(L"SystemRoot").value_or("C:\\Windows") / "System32" / "cmd.exe";
        }

        struct Presets
        {
            std::vector<std::string> mConfigure;
            std::vector<std::string> mHidden;
            // build preset -> the configure preset it builds
            std::map<std::string, std::string, std::less<>> mBuild;
            bool mFound = false;
        };

        bool IsHidden(const Json &preset)
        {
            const auto hidden = preset.find("hidden");
            return hidden != preset.end() && hidden->is_boolean() && hidden->get<bool>();
        }

        std::string StringField(const Json &object, const char *key)
        {
            const auto it = object.find(key);
            return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
        }

        void CollectPresets(const fs::path &file, Presets &presets, int depth)
        {
            const std::optional<Json> json = ReadJsonFile(file);
            if (depth > 8 || !json || !json->is_object())
                return;
            presets.mFound = true;

            if (const auto list = json->find("configurePresets"); list != json->end() && list->is_array())
            {
                for (const Json &preset : *list)
                {
                    const std::string name = preset.is_object() ? StringField(preset, "name") : std::string();
                    if (!name.empty())
                        (IsHidden(preset) ? presets.mHidden : presets.mConfigure).push_back(name);
                }
            }
            if (const auto list = json->find("buildPresets"); list != json->end() && list->is_array())
            {
                for (const Json &preset : *list)
                {
                    const std::string name = preset.is_object() ? StringField(preset, "name") : std::string();
                    if (!name.empty() && !IsHidden(preset))
                        presets.mBuild.emplace(name, StringField(preset, "configurePreset"));
                }
            }
            if (const auto includes = json->find("include"); includes != json->end() && includes->is_array())
            {
                for (const Json &include : *includes)
                {
                    if (include.is_string())
                        CollectPresets(file.parent_path() / FromText(include.get_ref<const std::string &>()), presets,
                            depth + 1);
                }
            }
        }

        std::string FailureText(int exit, const GameBuildCompile &compile, const fs::path &log)
        {
            std::string what;
            switch (exit)
            {
            case kGameBuildExitNoCheckout:
                what = "could not enter the checkout " + PathText(compile.mSourceRoot);
                break;
            case kGameBuildExitConfigure:
                what = "configuring " + compile.mPreset + " failed";
                break;
            case kGameBuildExitCompile:
                what = "compiling the game build with " + compile.mPreset + " failed";
                break;
            case kGameBuildExitInstall:
                what = "installing the game build from " + PathText(PresetBuildDir(compile.mSourceRoot, compile.mPreset)) +
                    " failed";
                break;
            default:
                what = std::string(kScriptFileName) + " stopped with exit code " + std::to_string(exit);
                break;
            }
            what += "; its output is in " + PathText(log);
            if (!EnvironmentPath(L"VCToolsInstallDir"))
                what += ". This is not a Developer Command Prompt, which the compile needs";
            return what;
        }
    }

    fs::path PresetBuildDir(const fs::path &sourceRoot, std::string_view preset)
    {
        return sourceRoot / "builds" / FromText(preset);
    }

    fs::path GameBuildStagingDir(const fs::path &gameBuildDir)
    {
        fs::path staging = NormalizedPath(gameBuildDir);
        staging += ".tmp";
        return staging;
    }

    std::optional<std::string> PresetProblem(const fs::path &sourceRoot, std::string_view preset)
    {
        if (preset.empty())
            return "the game build preset is empty";
        const bool plain = std::ranges::all_of(preset, [](unsigned char c) {
            return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.';
        });
        if (!plain || preset.starts_with('.'))
            return "the game build preset \"" + std::string(preset) + "\" may hold only letters, digits, '-', '_' and '.'";

        Presets presets;
        for (std::string_view file : { "CMakePresets.json", "CMakeUserPresets.json" })
            CollectPresets(sourceRoot / file, presets, 0);
        if (!presets.mFound)
            return PathText(sourceRoot) + " has no readable CMakePresets.json, so the game build cannot be compiled from it";

        const std::string name(preset);
        if (std::ranges::find(presets.mHidden, name) != presets.mHidden.end())
            return "the preset " + name + " is hidden, so CMake cannot configure with it";
        if (std::ranges::find(presets.mConfigure, name) == presets.mConfigure.end())
        {
            std::string known;
            for (const std::string &other : presets.mConfigure)
                known += (known.empty() ? "" : ", ") + other;
            return "CMakePresets.json and CMakeUserPresets.json define no preset named " + name + "; they define " +
                (known.empty() ? "none" : known);
        }

        const auto build = presets.mBuild.find(name);
        if (build == presets.mBuild.end())
            return "the preset " + name + " has no build preset of the same name, which cmake --build --preset needs";
        if (build->second != name)
            return "the build preset " + name + " builds the configure preset " + build->second +
                ", so the game build would not compile in " + PathText(PresetBuildDir(sourceRoot, preset));
        return std::nullopt;
    }

    std::optional<std::string> GameBuildDirProblem(const fs::path &dir)
    {
        std::error_code ec;
        if (!fs::exists(dir, ec))
            return std::nullopt;
        if (!fs::is_directory(dir, ec))
            return PathText(dir) + " is a file, so the game build cannot be installed there";
        if (fs::is_empty(dir, ec))
            return std::nullopt;
        if (fs::exists(dir / "CMakeCache.txt", ec))
            return PathText(dir) + " is a CMake build folder; pick another folder for the game build template";
        if (IsBuildDir(dir))
            return PathText(dir) + " is a packaged game; pick another folder for the game build template";

        const std::optional<Json> info = ReadJsonFile(dir / kGameBuildInfoFileName);
        if (!info || !info->is_object() || GameBuildInfoFlag(*info, "editor") != false)
            return PathText(dir) + " holds files that are not a game build template, so a compile does not replace "
                "it; empty it or pick another folder";
        return std::nullopt;
    }

    std::optional<std::string> CheckGameBuildCompile(const GameBuildCompile &compile, const fs::path &outDir)
    {
        if (std::optional<std::string> problem = PresetProblem(compile.mSourceRoot, compile.mPreset))
            return problem;

        const fs::path buildDir = NormalizedPath(PresetBuildDir(compile.mSourceRoot, compile.mPreset));
        for (const fs::path &own : { ExecutablePath(), compile.mEditorInfo })
        {
            if (!own.empty() && IsWithin(NormalizedPath(own), buildDir))
                return PathText(buildDir) + " is the build folder this MjolnirBuild and its editor come from; "
                    "compiling the game build there could rebuild them while they run, so use a game preset such as "
                    "windows-msvc-game-release";
        }
        for (const auto &[dir, what] : { std::pair{ NormalizedPath(compile.mGameBuildDir), "the game build template " },
                 std::pair{ NormalizedPath(outDir), "the output folder " } })
        {
            if (IsWithin(dir, buildDir) || IsWithin(buildDir, dir))
                return what + PathText(dir) + " overlaps " + PathText(buildDir) +
                    ", where the game build compiles; use a folder of its own";
        }

        if (std::optional<std::string> problem = GameBuildDirProblem(compile.mGameBuildDir))
            return problem;
        const fs::path staging = GameBuildStagingDir(compile.mGameBuildDir);
        if (!IsOwnStaging(staging))
        {
            if (std::optional<std::string> problem = GameBuildDirProblem(staging))
                return *problem + " (the game build is installed there before it replaces the template)";
        }
        return std::nullopt;
    }

    std::vector<std::string> FeatureDefines(const Json &editorInfo)
    {
        std::vector<std::string> defines;
        for (const GameBuildFeature &feature : kGameBuildFeatures)
        {
            if (const std::optional<bool> on = GameBuildInfoFlag(editorInfo, feature.mKey))
                defines.push_back("-D" + std::string(feature.mOption) + "=" + (*on ? "ON" : "OFF"));
        }
        return defines;
    }

    bool CacheMatches(const fs::path &cacheFile, std::span<const std::string> defines)
    {
        const std::optional<std::string> cache = ReadText(cacheFile);
        if (!cache)
            return false;

        for (const std::string &define : defines)
        {
            const std::size_t equals = define.find('=');
            if (!define.starts_with("-D") || equals == std::string::npos)
                return false;
            const std::string_view wanted = std::string_view(define).substr(equals + 1);
            const std::optional<std::string_view> value =
                CacheValue(*cache, std::string_view(define).substr(2, equals - 2));
            if (!value)
                return false;

            const std::optional<bool> wantedOn = CMakeBool(wanted);
            const std::optional<bool> valueOn = CMakeBool(*value);
            if (wantedOn && valueOn ? *wantedOn != *valueOn : *value != wanted)
                return false;
        }
        return true;
    }

    bool NeedsConfigure(const fs::path &buildDir, std::span<const std::string> defines)
    {
        // a configure that failed leaves a cache, but no build.ninja
        std::error_code ec;
        return !fs::is_regular_file(buildDir / "build.ninja", ec) || !CacheMatches(buildDir / "CMakeCache.txt", defines);
    }

    std::string GameBuildScriptText(const GameBuildScript &script)
    {
        const auto exitWith = [](int code) { return " || exit /b " + std::to_string(code) + "\r\n"; };

        std::string text = "@echo off\r\nchcp 65001 >nul\r\n";
        text += "cd /d \"" + BatchPath(script.mSourceRoot) + "\"" + exitWith(kGameBuildExitNoCheckout);
        if (script.mConfigure)
        {
            text += "cmake --preset " + script.mPreset;
            for (const std::string &define : *script.mConfigure)
                text += " " + define;
            text += exitWith(kGameBuildExitConfigure);
        }
        text += "cmake --build --preset " + script.mPreset + " --target " + std::string(kGameBuildTargets) +
            exitWith(kGameBuildExitCompile);
        text += "cmake --install \"builds\\" + script.mPreset + "\" --prefix \"" + BatchPath(script.mPrefix) + "\"" +
            exitWith(kGameBuildExitInstall);
        return text;
    }

    void CompileGameBuild(const GameBuildCompile &compile)
    {
        if (std::optional<std::string> problem = PresetProblem(compile.mSourceRoot, compile.mPreset))
            throw std::runtime_error(*problem);
        if (std::optional<std::string> problem = GameBuildDirProblem(compile.mGameBuildDir))
            throw std::runtime_error(*problem);

        // Installed beside the template first, so a failure leaves the previous one in place
        const fs::path gameBuildDir = NormalizedPath(compile.mGameBuildDir);
        const fs::path staging = GameBuildStagingDir(gameBuildDir);
        std::error_code ec;
        if (!IsOwnStaging(staging))
        {
            if (std::optional<std::string> problem = GameBuildDirProblem(staging))
                throw std::runtime_error(*problem);
        }
        fs::remove_all(staging, ec);
        if (ec)
            throw std::runtime_error("could not remove the unfinished install " + PathText(staging) + ": " + ec.message());
        fs::create_directories(staging, ec);
        if (ec || !WriteText(staging / kStagingMarkerFileName, ""))
            throw std::runtime_error("could not create " + PathText(staging));

        try
        {
            std::vector<std::string> defines;
            if (!compile.mEditorInfo.empty())
            {
                if (const std::optional<Json> info = ReadJsonFile(compile.mEditorInfo); info && info->is_object())
                    defines = FeatureDefines(*info);
            }

            const fs::path buildDir = PresetBuildDir(compile.mSourceRoot, compile.mPreset);
            GameBuildScript script{ compile.mSourceRoot, compile.mPreset, staging, std::nullopt };
            if (NeedsConfigure(buildDir, defines))
                script.mConfigure = defines;

            // Kept, so the same commands can be run again by hand
            fs::create_directories(buildDir, ec);
            if (ec || !WriteText(buildDir / kScriptFileName, GameBuildScriptText(script)))
                throw std::runtime_error("could not write " + PathText(buildDir / kScriptFileName));

            const fs::path log = buildDir / kLogFileName;
            // .\ because NoDefaultCurrentDirectoryInExePath stops cmd looking in its own folder for a bare name
            const std::vector<std::string> args = { "/d", "/c", ".\\" + std::string(kScriptFileName) };
            ChildProcess process;
            if (!process.Start(CommandShell(), args, buildDir, log))
                throw std::runtime_error("could not start " + PathText(CommandShell()) + " to compile the game build");
            const std::optional<int> exit = process.Wait();
            if (!exit)
                throw std::runtime_error("lost track of " + std::string(kScriptFileName));
            if (*exit != 0)
                throw std::runtime_error(FailureText(*exit, compile, log));

            if (std::optional<std::string> problem = GameBuildTemplateProblem(staging))
                throw std::runtime_error(PathText(buildDir) + " installed no game build: " + *problem +
                    "; use a game preset such as windows-msvc-game-release");
        }
        catch (...)
        {
            fs::remove_all(staging, ec);
            throw;
        }

        fs::remove(staging / kStagingMarkerFileName, ec);
        fs::remove_all(gameBuildDir, ec);
        if (ec)
            throw std::runtime_error("could not remove the previous game build template " + PathText(gameBuildDir) + ": " +
                ec.message());
        fs::create_directories(gameBuildDir.parent_path(), ec);
        fs::rename(staging, gameBuildDir, ec);
        if (ec)
            throw std::runtime_error("could not move the new game build into " + PathText(gameBuildDir) + ": " + ec.message());
    }
}
