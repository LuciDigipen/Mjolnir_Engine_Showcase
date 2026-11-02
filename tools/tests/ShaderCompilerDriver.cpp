/**
 * @file ShaderCompilerDriver.cpp
 * @author Jackey Cen
 * @brief Controlled compiler process for shader publication regression tests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 */
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace
{
    std::string Read(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }
    int Run(const std::vector<std::string> &args)
    {
        if (args.size() < 2) return 2;
        std::map<std::string, std::string> options;
        for (size_t i = 2; i + 1 < args.size(); ++i)
            if (args[i] == "-o" || args[i] == "-reflection-json" || args[i] == "-depfile")
            {
                const auto flag = args[i];
                options[flag] = args[++i];
            }
        const auto source = std::filesystem::u8path(args[1]);
        const auto text = Read(source);
        std::ofstream(source.string() + ".started") << text;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::filesystem::exists(source.string() + ".hold"))
        {
            if (std::chrono::steady_clock::now() > deadline) return 3;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (text.find("fail") != std::string::npos) return 7;
        uint32_t hash = 2166136261u;
        for (const unsigned char byte : text) hash = (hash ^ byte) * 16777619u;
        const std::vector<uint32_t> words{0x07230203, 0x00010500, hash, 2, 0,
            (6u << 16) | 15u, 5, 1, 0x626f7270, 0x69614d65, 0x0000006e};
        std::ofstream spirv(std::filesystem::u8path(options.at("-o")), std::ios::binary);
        spirv.write(reinterpret_cast<const char *>(words.data()), static_cast<std::streamsize>(words.size() * sizeof(uint32_t)));
        std::ofstream(std::filesystem::u8path(options.at("-reflection-json"))) << "{}\n";
        std::ofstream(std::filesystem::u8path(options.at("-depfile"))) << "out: " << args[1] << '\n';
        return 0;
    }
}
#if defined(_WIN32)
int wmain(int argc, wchar_t **argv)
{
    std::vector<std::string> arguments;
    for (int i = 0; i < argc; ++i)
    {
        const int length = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string text(static_cast<size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, text.data(), length, nullptr, nullptr);
        text.pop_back();
        arguments.push_back(std::move(text));
    }
    return Run(arguments);
}
#else
int main(int argc, char **argv) { return Run({argv, argv + argc}); }
#endif
