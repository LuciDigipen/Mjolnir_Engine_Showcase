/**
 * @file FlightModelTemplates.h
 * @author Jackey Cen
 * @brief The built-in flight models a new settings file starts from.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <string_view>

namespace mir
{
    enum class FlightModelTemplate
    {
        Powered,
        Glider,
    };

    // The template's JSON text; never read from another project's directory.
    std::string_view FlightModelTemplateJson(FlightModelTemplate kind);
    std::string_view FlightModelTemplateName(FlightModelTemplate kind);
} // namespace mir
