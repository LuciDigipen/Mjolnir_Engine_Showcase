/**
 * @file CompileLog.cpp
 * @author Quy Dong Ta
 * @brief Phase markers, Ninja's [done/total] lines and compiler, linker and CMake errors in the compile log.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "CompileLog.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <utility>

namespace mir::build
{
    namespace
    {
        constexpr std::size_t kMaxErrorLength = 300;

        std::string_view Trim(std::string_view text)
        {
            const std::size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos)
                return {};
            return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        }

        std::vector<std::string_view> Lines(std::string_view text, bool completeOnly)
        {
            std::vector<std::string_view> lines;
            while (!text.empty())
            {
                const std::size_t end = text.find('\n');
                if (end == std::string_view::npos)
                {
                    if (!completeOnly)
                        lines.push_back(text);
                    break;
                }
                lines.push_back(text.substr(0, end));
                text.remove_prefix(end + 1);
            }
            return lines;
        }

        // [412/1310] at the start of the line
        std::optional<std::pair<std::size_t, std::size_t>> NinjaStatus(std::string_view line)
        {
            if (!line.starts_with('['))
                return std::nullopt;
            const char *const end = line.data() + line.size();
            std::size_t done = 0;
            std::size_t total = 0;
            const auto [slash, doneError] = std::from_chars(line.data() + 1, end, done);
            if (doneError != std::errc() || slash == end || *slash != '/')
                return std::nullopt;
            const auto [close, totalError] = std::from_chars(slash + 1, end, total);
            if (totalError != std::errc() || close == end || *close != ']')
                return std::nullopt;
            return std::pair{ done, total };
        }

        // "error C2065" or "error LNK2019", not inside a longer word
        bool HasCode(std::string_view line, std::string_view prefix)
        {
            for (std::size_t at = line.find(prefix); at != std::string_view::npos; at = line.find(prefix, at + 1))
            {
                const std::size_t after = at + prefix.size();
                const bool wordStart = at == 0 || !std::isalnum(static_cast<unsigned char>(line[at - 1]));
                if (wordStart && after < line.size() && std::isdigit(static_cast<unsigned char>(line[after])))
                    return true;
            }
            return false;
        }

        
        std::string Shortened(std::string_view text)
        {
            if (text.size() <= kMaxErrorLength)
                return std::string(text);
            std::size_t cut = kMaxErrorLength;
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
                --cut;
            return std::string(text.substr(0, cut)) + "...";
        }
    }

    CompileProgress LogProgress(std::string_view log)
    {
        CompileProgress progress;
        for (const std::string_view raw : Lines(log, true))
        {
            const std::string_view line = Trim(raw);
            if (line.starts_with(kPhaseMarker))
            {
                progress = { std::string(Trim(line.substr(kPhaseMarker.size()))) };
                continue;
            }
            if (progress.mPhase != "compile")
                continue;
            if (const auto status = NinjaStatus(line))
            {
                progress.mDone = status->first;
                progress.mTotal = status->second;
            }
        }
        return progress;
    }

    std::vector<std::string> LogErrors(std::string_view log, std::size_t max)
    {
        std::vector<std::string> errors;
        const std::vector<std::string_view> lines = Lines(log, false);
        for (std::size_t i = 0; i < lines.size() && errors.size() < max; ++i)
        {
            const std::string_view line = Trim(lines[i]);
            std::string error;
            if (line.starts_with("CMake Error"))
            {
                error = line;
                if (i + 1 < lines.size() && !Trim(lines[i + 1]).empty())
                    error += " " + std::string(Trim(lines[i + 1]));
            }
            else if (HasCode(line, "error C") || HasCode(line, "error LNK") || line.starts_with("[ERROR:"))
                error = line;
            else
                continue;

            error = Shortened(error);
            if (std::ranges::find(errors, error) == errors.end())
                errors.push_back(std::move(error));
        }
        return errors;
    }
}
