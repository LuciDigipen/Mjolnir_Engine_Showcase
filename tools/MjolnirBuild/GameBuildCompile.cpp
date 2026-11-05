/**
 * @file GameBuildCompile.cpp
 * @author Quy Dong Ta
 * @brief The game build step: writes mjolnir_game_build.bat into the preset's binaryDir, runs it with its output in
 *        mjolnir_game_build.log, and swaps the fresh install into the template's place.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "GameBuildCompile.h"

#include "BuildOutput.h"
#include "VisualStudio.h"

#include <core/fs/FileIO.h>
#include <core/fs/Paths.h>
#include <core/platform/Process.h>
#include <project/GameBuildInfo.h>
#include <serial/JsonFile.h>

#include <algorithm>
#include <cctype>
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
        // In a previous template moved aside, for the same reason
        constexpr std::string_view kPreviousMarkerFileName = "mjolnir_game_build_previous";

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

        bool IsMarked(const fs::path &dir, std::string_view marker)
        {
            std::error_code ec;
            return fs::is_regular_file(dir / marker, ec);
        }

        // The marker goes last, so a removal a locked file stopped still marks the folder as ours
        void RemoveMarked(const fs::path &dir, std::string_view marker, std::error_code &ec)
        {
            std::vector<fs::path> entries;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            {
                if (it->path().filename() != marker)
                    entries.push_back(it->path());
            }
            if (ec && ec != std::errc::no_such_file_or_directory)
                return;
            for (const fs::path &entry : entries)
            {
                fs::remove_all(entry, ec);
                if (ec)
                    return;
            }
            fs::remove_all(dir, ec);
        }

        constexpr std::string_view kNoVisualStudio = "no Visual Studio with the C++ tools was found; install the "
                                                     "\"Desktop development with C++\" workload with the Visual Studio "
                                                     "Installer";

        fs::path EditorBuildDir(const GameBuildCompile &compile)
        {
            return compile.mEditorInfo.empty() ? fs::path() : compile.mEditorInfo.parent_path();
        }

        fs::path CommandShell()
        {
            if (const std::optional<fs::path> comspec = EnvironmentPath(L"ComSpec"))
                return *comspec;
            return EnvironmentPath(L"SystemRoot").value_or("C:\\Windows") / "System32" / "cmd.exe";
        }

        struct ConfigurePreset
        {
            Json mJson;
            // for ${fileDir}
            fs::path mFileDir;
        };

        struct Presets
        {
            std::vector<std::string> mConfigure;
            std::vector<std::string> mHidden;
            std::map<std::string, ConfigurePreset, std::less<>> mByName;
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
                    if (name.empty())
                        continue;
                    (IsHidden(preset) ? presets.mHidden : presets.mConfigure).push_back(name);
                    presets.mByName.emplace(name, ConfigurePreset{ preset, file.parent_path() });
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

        Presets LoadPresets(const fs::path &sourceRoot)
        {
            Presets presets;
            for (std::string_view file : { "CMakePresets.json", "CMakeUserPresets.json" })
                CollectPresets(sourceRoot / file, presets, 0);
            return presets;
        }

        // The preset, or the first it inherits from depth first, that sets binaryDir
        const ConfigurePreset *FindBinaryDir(const Presets &presets, std::string_view name, int depth)
        {
            const auto it = presets.mByName.find(name);
            if (depth > 16 || it == presets.mByName.end())
                return nullptr;
            const Json &json = it->second.mJson;
            if (const auto dir = json.find("binaryDir"); dir != json.end() && dir->is_string())
                return &it->second;

            const auto inherits = json.find("inherits");
            if (inherits == json.end())
                return nullptr;
            const Json parents = inherits->is_array() ? *inherits : Json::array({ *inherits });
            for (const Json &parent : parents)
            {
                if (!parent.is_string())
                    continue;
                if (const ConfigurePreset *found = FindBinaryDir(presets, parent.get_ref<const std::string &>(), depth + 1))
                    return found;
            }
            return nullptr;
        }

        struct BuildDirResult
        {
            fs::path mDir;
            std::string mProblem;
        };

        // Expands the macros a binaryDir usually holds; $env{} and the rest are refused, not guessed
        BuildDirResult ResolveBuildDir(const Presets &presets, const fs::path &sourceRoot, const std::string &name)
        {
            const ConfigurePreset *owner = FindBinaryDir(presets, name, 0);
            if (!owner)
                return { {}, "the preset " + name + " sets no binaryDir, so its build folder is unknown" };

            const fs::path source = NormalizedPath(sourceRoot);
            const std::map<std::string, std::string, std::less<>> macros = {
                { "sourceDir", PathText(source) },
                { "sourceParentDir", PathText(source.parent_path()) },
                { "sourceDirName", PathText(source.filename()) },
                { "presetName", name },
                { "fileDir", PathText(NormalizedPath(owner->mFileDir)) },
                { "hostSystemName", "Windows" },
                { "dollar", "$" },
            };

            const std::string &text = owner->mJson.at("binaryDir").get_ref<const std::string &>();
            std::string expanded;
            for (std::size_t at = 0; at < text.size();)
            {
                const std::size_t dollar = text.find('$', at);
                expanded += text.substr(at, dollar - at);
                if (dollar == std::string::npos)
                    break;
                const std::size_t close = text.find('}', dollar);
                const auto macro = text.compare(dollar, 2, "${") == 0 && close != std::string::npos
                    ? macros.find(std::string_view(text).substr(dollar + 2, close - dollar - 2))
                    : macros.end();
                if (macro == macros.end())
                    return { {}, "the preset " + name + "'s binaryDir \"" + text +
                        "\" uses a macro MjolnirBuild does not expand; use ${sourceDir}, ${presetName} and the like" };
                expanded += macro->second;
                at = close + 1;
            }

            fs::path dir = FromText(expanded);
            if (dir.is_relative())
                dir = source / dir;
            return { NormalizedPath(dir), {} };
        }

        std::string FailureText(int exit, const GameBuildCompile &compile, const GameBuildScript &script,
            const fs::path &log)
        {
            std::string what;
            switch (exit)
            {
            case kGameBuildExitVisualStudio:
                what = "loading Visual Studio with " + PathText(script.mVisualStudio.mVcvars) + " failed";
                break;
            case kGameBuildExitNoCompiler:
                what = "cl.exe, the C++ compiler, is not on PATH; install the \"Desktop development with C++\" "
                       "workload with the Visual Studio Installer";
                break;
            case kGameBuildExitNoCMake:
                what = "cmake or ninja is not on PATH; install \"C++ CMake tools for Windows\" with the Visual Studio "
                       "Installer";
                break;
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
                what = "installing the game build from " + PathText(script.mBuildDir) + " failed";
                break;
            default:
                what = std::string(kScriptFileName) + " stopped with exit code " + std::to_string(exit);
                break;
            }
            what += "; its output is in " + PathText(log);
            return what;
        }

        // The build folder, the template with its .tmp and .old, and the output with its own: none may hold another
        std::optional<std::string> OverlapProblem(const fs::path &buildDir, const fs::path &gameBuildDir,
            const fs::path &outDir)
        {
            struct Folder
            {
                fs::path mDir;
                std::string mWhat;
                int mGroup;
            };
            const fs::path gameBuild = NormalizedPath(gameBuildDir);
            std::vector<Folder> folders = {
                { buildDir, "the preset's build folder ", 0 },
                { gameBuild, "the game build template ", 1 },
                { GameBuildStagingDir(gameBuild), "the template's install folder ", 1 },
                { GameBuildPreviousDir(gameBuild), "the folder the previous template moves to ", 1 },
            };
            if (!outDir.empty())
            {
                folders.push_back({ NormalizedPath(outDir), "the output folder ", 2 });
                folders.push_back({ StagingDir(outDir), "the output's staging folder ", 2 });
                folders.push_back({ PreviousDir(outDir), "the folder the previous output moves to ", 2 });
            }

            for (std::size_t i = 0; i < folders.size(); ++i)
            {
                for (std::size_t j = i + 1; j < folders.size(); ++j)
                {
                    const Folder &a = folders[i];
                    const Folder &b = folders[j];
                    if (a.mGroup != b.mGroup && (IsWithin(a.mDir, b.mDir) || IsWithin(b.mDir, a.mDir)))
                        return a.mWhat + PathText(a.mDir) + " overlaps " + b.mWhat + PathText(b.mDir) +
                            "; give each a folder of its own";
                }
            }
            return std::nullopt;
        }
    }

    std::optional<fs::path> PresetBuildDir(const fs::path &sourceRoot, std::string_view preset)
    {
        BuildDirResult result = ResolveBuildDir(LoadPresets(sourceRoot), sourceRoot, std::string(preset));
        if (!result.mProblem.empty())
            return std::nullopt;
        return std::move(result.mDir);
    }

    fs::path GameBuildStagingDir(const fs::path &gameBuildDir)
    {
        fs::path staging = NormalizedPath(gameBuildDir);
        staging += ".tmp";
        return staging;
    }

    fs::path GameBuildPreviousDir(const fs::path &gameBuildDir)
    {
        fs::path previous = NormalizedPath(gameBuildDir);
        previous += ".old";
        return previous;
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

        const Presets presets = LoadPresets(sourceRoot);
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

        const BuildDirResult buildDir = ResolveBuildDir(presets, sourceRoot, name);
        if (!buildDir.mProblem.empty())
            return buildDir.mProblem;

        const auto build = presets.mBuild.find(name);
        if (build == presets.mBuild.end())
            return "the preset " + name + " has no build preset of the same name, which cmake --build --preset needs";
        if (build->second != name)
            return "the build preset " + name + " builds the configure preset " + build->second +
                ", so the game build would not compile in " + PathText(buildDir.mDir);
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

        const std::optional<fs::path> found = PresetBuildDir(compile.mSourceRoot, compile.mPreset);
        if (!found)
            return "the build folder of the preset " + compile.mPreset + " is unknown";
        const fs::path &buildDir = *found;
        for (const fs::path &own : { ExecutablePath(), compile.mEditorInfo })
        {
            if (!own.empty() && IsWithin(NormalizedPath(own), buildDir))
                return PathText(buildDir) + " is the build folder this MjolnirBuild and its editor come from; "
                    "compiling the game build there could rebuild them while they run, so use a game preset such as "
                    "windows-msvc-game-release";
        }
        if (std::optional<std::string> problem = OverlapProblem(buildDir, compile.mGameBuildDir, outDir))
            return problem;

        if (std::optional<std::string> problem = GameBuildDirProblem(compile.mGameBuildDir))
            return problem;
        const fs::path staging = GameBuildStagingDir(compile.mGameBuildDir);
        if (!IsMarked(staging, kStagingMarkerFileName))
        {
            if (std::optional<std::string> problem = GameBuildDirProblem(staging))
                return *problem + " (the game build is installed there before it replaces the template)";
        }
        const fs::path previous = GameBuildPreviousDir(compile.mGameBuildDir);
        if (!IsMarked(previous, kPreviousMarkerFileName))
        {
            if (std::optional<std::string> problem = GameBuildDirProblem(previous))
                return *problem + " (the previous template is moved there while the new one replaces it)";
        }

        if (!FindVisualStudio(buildDir, EditorBuildDir(compile)))
            return std::string(kNoVisualStudio);
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

    std::string GameBuildScriptText(const GameBuildScript &script)
    {
        const auto exitWith = [](int code) { return " || exit /b " + std::to_string(code) + "\r\n"; };

        std::string text = "@echo off\r\nchcp 65001 >nul\r\n";
        const VisualStudio &visualStudio = script.mVisualStudio;
        if (!visualStudio.mVcvars.empty())
        {
            // vcvars looks for vswhere on PATH, and says so in the log when it is missing
            text += "set \"PATH=%PATH%;%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\"\r\n";
            text += "call \"" + BatchPath(visualStudio.mVcvars) + "\"";
            if (!visualStudio.mToolset.empty())
                text += " -vcvars_ver=" + visualStudio.mToolset;
            text += exitWith(kGameBuildExitVisualStudio);
        }
        text += "where /q cl" + exitWith(kGameBuildExitNoCompiler);
        text += "where /q cmake" + exitWith(kGameBuildExitNoCMake);
        text += "where /q ninja" + exitWith(kGameBuildExitNoCMake);
        // after vcvars, which may change folder
        text += "cd /d \"" + BatchPath(script.mSourceRoot) + "\"" + exitWith(kGameBuildExitNoCheckout);
        text += "cmake --preset " + script.mPreset;
        for (const std::string &define : script.mDefines)
            text += " " + define;
        text += exitWith(kGameBuildExitConfigure);
        text += "cmake --build --preset " + script.mPreset + " --target " + std::string(kGameBuildTargets) +
            exitWith(kGameBuildExitCompile);
        text += "cmake --install \"" + BatchPath(script.mBuildDir) + "\" --prefix \"" + BatchPath(script.mPrefix) + "\"" +
            exitWith(kGameBuildExitInstall);
        return text;
    }

    void CompileGameBuild(const GameBuildCompile &compile)
    {
        if (std::optional<std::string> problem = PresetProblem(compile.mSourceRoot, compile.mPreset))
            throw std::runtime_error(*problem);
        if (std::optional<std::string> problem = GameBuildDirProblem(compile.mGameBuildDir))
            throw std::runtime_error(*problem);
        const std::optional<fs::path> found = PresetBuildDir(compile.mSourceRoot, compile.mPreset);
        if (!found)
            throw std::runtime_error("the build folder of the preset " + compile.mPreset + " is unknown");
        const fs::path &buildDir = *found;
        if (std::optional<std::string> problem = OverlapProblem(buildDir, compile.mGameBuildDir, {}))
            throw std::runtime_error(*problem);
        const std::optional<VisualStudio> visualStudio = FindVisualStudio(buildDir, EditorBuildDir(compile));
        if (!visualStudio)
            throw std::runtime_error(std::string(kNoVisualStudio));

        // Installed beside the template first, so a failure leaves the previous one in place
        const fs::path gameBuildDir = NormalizedPath(compile.mGameBuildDir);
        const fs::path staging = GameBuildStagingDir(gameBuildDir);
        const fs::path previous = GameBuildPreviousDir(gameBuildDir);
        std::error_code ec;
        for (const auto &[dir, marker] : { std::pair{ staging, kStagingMarkerFileName },
                 std::pair{ previous, kPreviousMarkerFileName } })
        {
            if (!IsMarked(dir, marker))
            {
                if (std::optional<std::string> problem = GameBuildDirProblem(dir))
                    throw std::runtime_error(*problem);
                fs::remove_all(dir, ec);
            }
            else
                RemoveMarked(dir, marker, ec);
            if (ec)
                throw std::runtime_error("could not remove " + PathText(dir) + ", left by an earlier compile: " +
                    ec.message());
        }
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

            const GameBuildScript script{ compile.mSourceRoot, compile.mPreset, buildDir, staging, defines, *visualStudio };

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
                throw std::runtime_error(FailureText(*exit, compile, script, log));

            if (std::optional<std::string> problem = GameBuildTemplateProblem(staging))
                throw std::runtime_error(PathText(buildDir) + " installed no game build: " + *problem +
                    "; use a game preset such as windows-msvc-game-release");
        }
        catch (...)
        {
            RemoveMarked(staging, kStagingMarkerFileName, ec);
            throw;
        }

        // Moved aside rather than deleted, so a locked file cannot leave the previous template half removed
        std::error_code ignored;
        bool movedAside = false;
        if (fs::exists(gameBuildDir, ec))
        {
            fs::rename(gameBuildDir, previous, ec);
            if (ec)
            {
                const std::string what = "could not move the previous game build template " + PathText(gameBuildDir) +
                    " aside (" + ec.message() + "); is a game running from it?";
                RemoveMarked(staging, kStagingMarkerFileName, ignored);
                throw std::runtime_error(what);
            }
            movedAside = true;
            WriteText(previous / kPreviousMarkerFileName, "");
        }

        fs::create_directories(gameBuildDir.parent_path(), ec);
        fs::rename(staging, gameBuildDir, ec);
        if (ec)
        {
            std::string what = "could not move the new game build into " + PathText(gameBuildDir) + ": " + ec.message();
            if (movedAside)
            {
                fs::rename(previous, gameBuildDir, ignored);
                if (ignored)
                    what += "; the previous template is in " + PathText(previous);
                else
                    fs::remove(gameBuildDir / kPreviousMarkerFileName, ignored);
            }
            RemoveMarked(staging, kStagingMarkerFileName, ignored);
            throw std::runtime_error(what);
        }
        fs::remove(gameBuildDir / kStagingMarkerFileName, ignored);

        // a failure leaves it marked, for the next compile to remove
        if (movedAside)
            RemoveMarked(previous, kPreviousMarkerFileName, ignored);
    }
}
