/**
 * @file RunGuarded.h
 * @author sumin.park
 * @brief Runs a worker job body and turns an exception into that job's error result
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <exception>
#include <new>
#include <string_view>

namespace mir
{
    // toError(what) builds the job's failed result and adds the call site's context.
    // std::bad_alloc terminates instead, reaching the crash handler: building an error
    // allocates too, and running out of memory is the process's problem, not the asset's
    template <typename Build, typename ToError>
    auto RunGuarded(Build &&build, ToError &&toError) noexcept -> decltype(build())
    {
        try
        {
            return build();
        }
        catch (const std::bad_alloc &)
        {
            std::terminate();
        }
        catch (const std::exception &e)
        {
            return toError(std::string_view(e.what()));
        }
        catch (...)
        {
            return toError(std::string_view("unknown exception"));
        }
    }
} // namespace mir
