/**
 * @file VisualStudio.cpp
 * @author Quy Dong Ta
 * @brief Reads the compiler a CMakeCache.txt was configured with, and asks vswhere for the newest Visual Studio.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "VisualStudio.h"

#include "BuildOutput.h"

#include <core/fs/FileIO.h>
#include <core/platform/Process.h>

#include <algorithm>
#include <cwctype>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace mir::build
{
    namespace fs = std::filesystem;

    namespace
    {
        std::string_view Trim(std::string_view text)
        {
            const std::size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos)
                return {};
            return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        }

        fs::path FromText(std::string_view text)
        {
            return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.data()), text.size()));
        }

        // NAME:TYPE=VALUE in CMakeCache.txt
        std::optional<std::string> CacheValue(const fs::path &cacheFile, std::string_view name)
        {
            const std::optional<std::string> cache = ReadText(cacheFile);
            if (!cache)
                return std::nullopt;
            for (std::string_view rest = *cache; !rest.empty();)
            {
                const std::size_t end = rest.find('\n');
                const std::string_view line = Trim(rest.substr(0, end));
                rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);

                if (line.size() > name.size() && line.starts_with(name) &&
                    (line[name.size()] == ':' || line[name.size()] == '='))
                {
                    const std::size_t equals = line.find('=');
                    if (equals != std::string_view::npos)
                        return std::string(line.substr(equals + 1));
                }
            }
            return std::nullopt;
        }

        bool SameName(const fs::path &part, std::wstring_view name)
        {
            const std::wstring text = part.wstring();
            return std::ranges::equal(text, name, [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
        }

        std::optional<VisualStudio> FromInstall(const fs::path &install, std::string toolset)
        {
            const fs::path vcvars = install / "VC" / "Auxiliary" / "Build" / kVcvarsFileName;
            std::error_code ec;
            if (!fs::is_regular_file(vcvars, ec))
                return std::nullopt;
            return VisualStudio{ vcvars, std::move(toolset) };
        }

        // The CMakeCache.txt of the build folder dir is in, which may be dir itself or above it
        fs::path BuildCacheAbove(const fs::path &dir)
        {
            std::error_code ec;
            for (fs::path at = NormalizedPath(dir);; at = at.parent_path())
            {
                if (fs::is_regular_file(at / "CMakeCache.txt", ec))
                    return at / "CMakeCache.txt";
                if (at == at.parent_path())
                    return {};
            }
        }
    }

    std::optional<fs::path> EnvironmentPath(const wchar_t *name)
    {
        const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
        if (size <= 1)
            return std::nullopt;
        std::wstring value(size, L'\0');
        value.resize(GetEnvironmentVariableW(name, value.data(), size));
        return value.empty() ? std::nullopt : std::optional(fs::path(value));
    }

    std::optional<VisualStudio> CacheVisualStudio(const fs::path &cacheFile)
    {
        const std::optional<std::string> value = CacheValue(cacheFile, "CMAKE_CXX_COMPILER");
        if (!value)
            return std::nullopt;
        const fs::path compiler = FromText(*value).make_preferred();
        std::error_code ec;
        if (!compiler.is_absolute() || !SameName(compiler.filename(), L"cl.exe") || !fs::is_regular_file(compiler, ec))
            return std::nullopt;

        // <install>/VC/Tools/MSVC/<toolset>/bin/Hostx64/x64/cl.exe
        const std::vector<fs::path> parts(compiler.begin(), compiler.end());
        for (std::size_t i = 0; i + 3 < parts.size(); ++i)
        {
            if (!SameName(parts[i], L"VC") || !SameName(parts[i + 1], L"Tools") || !SameName(parts[i + 2], L"MSVC"))
                continue;
            const std::string toolset = PathText(parts[i + 3]);
            if (toolset.empty() || !std::ranges::all_of(toolset, [](char c) { return (c >= '0' && c <= '9') || c == '.'; }))
                return std::nullopt;
            fs::path install;
            for (std::size_t j = 0; j < i; ++j)
                install /= parts[j];
            return FromInstall(install, toolset);
        }
        return std::nullopt;
    }

    std::optional<VisualStudio> NewestVisualStudio()
    {
        std::error_code ec;
        fs::path vswhere;
        for (const wchar_t *root : { L"ProgramFiles(x86)", L"ProgramFiles" })
        {
            if (const std::optional<fs::path> dir = EnvironmentPath(root))
            {
                const fs::path candidate = *dir / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
                if (fs::is_regular_file(candidate, ec))
                {
                    vswhere = candidate;
                    break;
                }
            }
        }
        if (vswhere.empty())
            return std::nullopt;

        const fs::path output = fs::temp_directory_path(ec) /
            ("mjolnir_vswhere_" + std::to_string(CurrentProcessId()) + ".txt");
        const std::vector<std::string> args = { "-latest", "-prerelease", "-products", "*", "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath", "-utf8", "-nologo" };
        ChildProcess process;
        const bool ran = process.Start(vswhere, args, {}, output) && process.Wait() == 0;
        const std::optional<std::string> text = ran ? ReadText(output) : std::nullopt;
        fs::remove(output, ec);
        if (!text)
            return std::nullopt;

        const std::string_view first = Trim(std::string_view(*text).substr(0, text->find('\n')));
        if (first.empty())
            return std::nullopt;
        return FromInstall(FromText(first), {});
    }

    std::optional<VisualStudio> FindVisualStudio(const fs::path &buildDir, const fs::path &editorDir)
    {
        if (EnvironmentPath(L"VCToolsInstallDir"))
            return VisualStudio{};
        if (std::optional<VisualStudio> own = CacheVisualStudio(buildDir / "CMakeCache.txt"))
            return own;
        if (!editorDir.empty())
        {
            if (std::optional<VisualStudio> editor = CacheVisualStudio(BuildCacheAbove(editorDir)))
                return editor;
        }
        return NewestVisualStudio();
    }
}
