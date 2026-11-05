/**
 * @file CompileLog.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for reading progress and errors from sample mjolnir_game_build.log files.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <CompileLog.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace mir::build;

namespace
{
    // As the bat, vcvars, vcpkg, CMake and Ninja write it, up to Ninja's 412th object
    constexpr std::string_view kCompiling =
        "mjolnir_game_build: environment\r\n"
        "**********************************************************************\r\n"
        "** Visual Studio 2026 Developer Command Prompt v18.0\r\n"
        "**********************************************************************\r\n"
        "[vcvarsall.bat] Environment initialized for: 'x64'\r\n"
        "mjolnir_game_build: configure\r\n"
        "-- Running vcpkg install\r\n"
        "[1/2] a status line before the compile, which is not Ninja's\r\n"
        "-- Build files have been written to: C:/alpha/builds/windows-msvc-game-release\r\n"
        "mjolnir_game_build: compile\r\n"
        "[1/1310] Building CXX object modules/core/CMakeFiles/core.dir/src/Log.cpp.obj\r\n"
        "[2/1310] Building CXX object modules/core/CMakeFiles/core.dir/src/Paths.cpp.obj\r\n"
        "[412/1310] Building CXX object modules/render/CMakeFiles/render.dir/src/Renderer.cpp.obj\r\n";
}

TEST_CASE("Progress is the last phase marker and Ninja's last [done/total] in it", "[compile-log]")
{
    CHECK(LogProgress("") == CompileProgress{});
    CHECK(LogProgress("mjolnir_game_build: environment\r\n") == CompileProgress{ "environment" });
    CHECK(LogProgress(std::string(kCompiling).substr(0, std::string(kCompiling).find("-- Build files"))) ==
        CompileProgress{ "configure" });
    CHECK(LogProgress(kCompiling) == CompileProgress{ "compile", 412, 1310 });

    // a line still being written is left for the next read
    CHECK(LogProgress(std::string(kCompiling) + "[413/13") == CompileProgress{ "compile", 412, 1310 });
    CHECK(LogProgress(std::string(kCompiling) + "[413/1310] Linking CXX executable MjolnirEngine.exe\n") ==
        CompileProgress{ "compile", 413, 1310 });

    // the next phase starts with no count
    CHECK(LogProgress(std::string(kCompiling) + "mjolnir_game_build: install\r\n-- Install configuration: \"Release\"\r\n") ==
        CompileProgress{ "install" });

    // Ninja with nothing to do prints no count
    CHECK(LogProgress("mjolnir_game_build: compile\r\nninja: no work to do.\r\n") == CompileProgress{ "compile" });
    CHECK(LogProgress("mjolnir_game_build: compile\r\n[x/3] y\r\n[4/] z\r\n [5/6] indented\r\n") ==
        CompileProgress{ "compile", 5, 6 });
}

TEST_CASE("A compiler error is quoted from the log", "[compile-log]")
{
    const std::string log = std::string(kCompiling) +
        "FAILED: modules/render/CMakeFiles/render.dir/src/Renderer.cpp.obj\r\n"
        "C:\\PROGRA~1\\MICROS~4\\18\\COMMUN~1\\VC\\Tools\\MSVC\\1451~1.362\\bin\\Hostx64\\x64\\cl.exe /nologo /TP "
        "-DMIR_WITH_AUDIO=1 /W4 /WX /Fo... -c C:\\alpha\\modules\\render\\src\\Renderer.cpp\r\n"
        "C:\\alpha\\modules\\render\\src\\Renderer.cpp(88): error C2065: 'frameIndex': undeclared identifier\r\n"
        "C:\\alpha\\modules\\render\\src\\Renderer.cpp(90): warning C4189: 'unused': local variable is initialized\r\n"
        "C:\\alpha\\modules\\render\\src\\Renderer.cpp(91): error C2065: 'frameIndex': undeclared identifier\r\n"
        "ninja: build stopped: subcommand failed.\r\n";

    CHECK(LogErrors(log) == std::vector<std::string>{
                                "C:\\alpha\\modules\\render\\src\\Renderer.cpp(88): error C2065: 'frameIndex': undeclared identifier",
                                "C:\\alpha\\modules\\render\\src\\Renderer.cpp(91): error C2065: 'frameIndex': undeclared identifier" });
    CHECK(LogErrors(kCompiling).empty());
}

TEST_CASE("Linker, vcvars and CMake errors are quoted, three at most and each once", "[compile-log]")
{
    const std::string linker =
        "Renderer.cpp.obj : error LNK2019: unresolved external symbol \"void __cdecl Draw(void)\" referenced in Frame\r\n"
        "Renderer.cpp.obj : error LNK2019: unresolved external symbol \"void __cdecl Draw(void)\" referenced in Frame\r\n"
        "LINK : fatal error LNK1104: cannot open file 'MjolnirEngine.exe'\r\n"
        "C:\\alpha\\a.cpp(1): fatal error C1083: Cannot open include file: 'missing.h': No such file or directory\r\n"
        "C:\\alpha\\b.cpp(2): error C2143: syntax error: missing ';' before '}'\r\n";
    CHECK(LogErrors(linker) == std::vector<std::string>{
                                   "Renderer.cpp.obj : error LNK2019: unresolved external symbol \"void __cdecl Draw(void)\" "
                                   "referenced in Frame",
                                   "LINK : fatal error LNK1104: cannot open file 'MjolnirEngine.exe'",
                                   "C:\\alpha\\a.cpp(1): fatal error C1083: Cannot open include file: 'missing.h': No such "
                                   "file or directory" });
    CHECK(LogErrors(linker, 1).size() == 1);

    // CMake's message is on the line after, and a CMake Error at the very end has none
    CHECK(LogErrors("-- Configuring incomplete\r\nCMake Error at CMakeLists.txt:12 (find_package):\r\n"
                    "  Could not find a package configuration file provided by \"fmt\".\r\n\r\n") ==
        std::vector<std::string>{
            "CMake Error at CMakeLists.txt:12 (find_package): Could not find a package configuration file provided by \"fmt\"." });
    CHECK(LogErrors("CMake Error: The source directory \"C:/nowhere\" does not exist.") ==
        std::vector<std::string>{ "CMake Error: The source directory \"C:/nowhere\" does not exist." });

    CHECK(LogErrors("[ERROR:vcvars.bat] Toolset directory for version '14.40' was not found.\r\n") ==
        std::vector<std::string>{ "[ERROR:vcvars.bat] Toolset directory for version '14.40' was not found." });

    // not errors: a word that ends in "error", a code with no number, and a warning
    CHECK(LogErrors("terror C2065\r\nerror C: none\r\nwarning C4189: unused\r\n").empty());
}

TEST_CASE("A long error line is shortened on a character boundary", "[compile-log]")
{
    // 2-byte UTF-8 characters after an odd-length start, so a cut at 300 bytes falls inside one
    std::string name;
    for (int i = 0; i < 200; ++i)
        name += "\xC3\xA9";
    const std::vector<std::string> errors = LogErrors("ab.cpp(1): error C2065: '" + name + "': undeclared identifier");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].size() <= 303);
    CHECK(errors[0].ends_with("..."));
    const std::string kept = errors[0].substr(0, errors[0].size() - 3);
    CHECK((static_cast<unsigned char>(kept.back()) & 0xC0) != 0xC0);
    CHECK(kept.size() % 2 == std::string("ab.cpp(1): error C2065: '").size() % 2);
}
