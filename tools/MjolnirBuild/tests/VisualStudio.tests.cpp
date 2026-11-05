/**
 * @file VisualStudio.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for finding the Visual Studio the game build step loads: from a CMakeCache.txt, then vswhere.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "BuildTestSupport.h"

#include <VisualStudio.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

using namespace mir;
using namespace mir::build;
using namespace mir::build::test;

namespace
{
    constexpr const char *kToolset = "14.51.36231";

    // An install with vcvars64.bat and one toolset's cl.exe; returns that cl.exe
    fs::path MakeVisualStudio(const fs::path &install, std::string_view toolset = kToolset)
    {
        Touch(install / "VC" / "Auxiliary" / "Build" / "vcvars64.bat", "@exit /b 0\r\n");
        const fs::path cl = install / "VC" / "Tools" / "MSVC" / toolset / "bin" / "Hostx64" / "x64" / "cl.exe";
        Touch(cl);
        return cl;
    }

    // As CMake writes it: forward slashes, after other entries that start the same
    void WriteCache(const fs::path &dir, const std::string &compiler)
    {
        Touch(dir / "CMakeCache.txt", "# This is the CMakeCache file.\r\n"
                                      "//CXX compiler\r\n"
                                      "CMAKE_CXX_COMPILER_AR:FILEPATH=C:/elsewhere/lib.exe\r\n"
                                      "CMAKE_CXX_COMPILER:FILEPATH=" + compiler + "\r\n"
                                      "CMAKE_C_COMPILER:FILEPATH=C:/other/cl.exe\r\n");
    }

    std::string CMakeText(const fs::path &path)
    {
        std::string text = PathText(path);
        std::ranges::replace(text, '\\', '/');
        return text;
    }
}

TEST_CASE("A CMakeCache.txt names the Visual Studio and toolset its compiler came from", "[visual-studio]")
{
    Sandbox sandbox;
    const fs::path install = sandbox.mRoot / "Microsoft Visual Studio" / "18" / "Community";
    const fs::path cl = MakeVisualStudio(install);
    WriteCache(sandbox.mRoot / "build", CMakeText(cl));

    const std::optional<VisualStudio> found = CacheVisualStudio(sandbox.mRoot / "build" / "CMakeCache.txt");
    REQUIRE(found);
    CHECK(found->mVcvars == install / "VC" / "Auxiliary" / "Build" / "vcvars64.bat");
    CHECK(found->mToolset == kToolset);

    // CMake may write the folders in another case
    std::string lower = CMakeText(cl);
    lower.replace(lower.find("/VC/Tools/MSVC/"), 15, "/vc/tools/msvc/");
    WriteCache(sandbox.mRoot / "lower", lower);
    REQUIRE(CacheVisualStudio(sandbox.mRoot / "lower" / "CMakeCache.txt"));
    CHECK(CacheVisualStudio(sandbox.mRoot / "lower" / "CMakeCache.txt")->mToolset == kToolset);
}

TEST_CASE("A CMakeCache.txt without an MSVC compiler that still exists names no Visual Studio", "[visual-studio]")
{
    Sandbox sandbox;
    const fs::path install = sandbox.mRoot / "VS";
    const fs::path cl = MakeVisualStudio(install);

    CHECK_FALSE(CacheVisualStudio(sandbox.mRoot / "none" / "CMakeCache.txt"));

    WriteCache(sandbox.mRoot / "bare", "cl");
    CHECK_FALSE(CacheVisualStudio(sandbox.mRoot / "bare" / "CMakeCache.txt"));

    const fs::path clang = sandbox.mRoot / "LLVM" / "bin" / "clang-cl.exe";
    Touch(clang);
    WriteCache(sandbox.mRoot / "clang", CMakeText(clang));
    CHECK_FALSE(CacheVisualStudio(sandbox.mRoot / "clang" / "CMakeCache.txt"));

    // a cl.exe outside VC/Tools/MSVC/<toolset>
    const fs::path loose = sandbox.mRoot / "tools" / "cl.exe";
    Touch(loose);
    WriteCache(sandbox.mRoot / "loose", CMakeText(loose));
    CHECK_FALSE(CacheVisualStudio(sandbox.mRoot / "loose" / "CMakeCache.txt"));

    // an uninstalled toolset, then an install without vcvars64.bat
    WriteCache(sandbox.mRoot / "gone", CMakeText(install / "VC" / "Tools" / "MSVC" / "14.40.33807" / "bin" / "Hostx64" /
                                                 "x64" / "cl.exe"));
    CHECK_FALSE(CacheVisualStudio(sandbox.mRoot / "gone" / "CMakeCache.txt"));
    WriteCache(sandbox.mRoot / "build", CMakeText(cl));
    REQUIRE(CacheVisualStudio(sandbox.mRoot / "build" / "CMakeCache.txt"));
    fs::remove(install / "VC" / "Auxiliary" / "Build" / "vcvars64.bat");
    CHECK_FALSE(CacheVisualStudio(sandbox.mRoot / "build" / "CMakeCache.txt"));
}

TEST_CASE("Visual Studio is found from the environment, the build folder, the editor's, then vswhere", "[visual-studio]")
{
    Sandbox sandbox;
    const fs::path build = sandbox.mRoot / "build";
    const fs::path editor = sandbox.mRoot / "editor";
    WriteCache(build, CMakeText(MakeVisualStudio(sandbox.mRoot / "Old VS", "14.38.33130")));
    WriteCache(editor, CMakeText(MakeVisualStudio(sandbox.mRoot / "New VS")));

    {
        const ScopedEnvironment developer(L"VCToolsInstallDir", L"C:\\VS\\VC\\Tools\\MSVC\\14.51.36231\\");
        const std::optional<VisualStudio> loaded = FindVisualStudio(build, editor);
        REQUIRE(loaded);
        CHECK(loaded->mVcvars.empty());
    }

    const ScopedEnvironment plain(L"VCToolsInstallDir", std::nullopt);
    // the build folder keeps its own, though the editor's is newer
    const std::optional<VisualStudio> own = FindVisualStudio(build, editor);
    REQUIRE(own);
    CHECK(own->mVcvars == sandbox.mRoot / "Old VS" / "VC" / "Auxiliary" / "Build" / "vcvars64.bat");
    CHECK(own->mToolset == "14.38.33130");

    // a first compile takes the editor's, found from a folder inside its build
    const std::optional<VisualStudio> first = FindVisualStudio(sandbox.mRoot / "fresh", editor / "bin");
    REQUIRE(first);
    CHECK(first->mVcvars == sandbox.mRoot / "New VS" / "VC" / "Auxiliary" / "Build" / "vcvars64.bat");
    CHECK(first->mToolset == "14.51.36231");

    const std::optional<VisualStudio> newest = NewestVisualStudio();
    const std::optional<VisualStudio> neither = FindVisualStudio(sandbox.mRoot / "fresh", sandbox.mRoot / "nowhere");
    CHECK(neither.has_value() == newest.has_value());
    if (neither && newest)
        CHECK(neither->mVcvars == newest->mVcvars);
}

TEST_CASE("vswhere finds an installed Visual Studio with the C++ tools", "[visual-studio]")
{
    const std::optional<VisualStudio> newest = NewestVisualStudio();
    if (!newest)
        SKIP("no Visual Studio with the C++ tools is installed");
    CHECK(fs::is_regular_file(newest->mVcvars));
    CHECK(newest->mVcvars.filename() == "vcvars64.bat");
    CHECK(newest->mToolset.empty());
}
