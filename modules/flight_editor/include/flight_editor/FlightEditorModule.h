/**
 * @file FlightEditorModule.h
 * @author Jackey Cen
 * @brief Editing *.aircraft.json files from the Content Browser, registered with the editor.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/Registration.h>
#include <core/ecs/ModuleContext.h>

#include <memory>

namespace mir
{
    class EditorModule;

    class FlightEditorModule
    {
    public:
        FlightEditorModule(ModuleContext context, EditorModule &editor);
        ~FlightEditorModule();

        FlightEditorModule(const FlightEditorModule &) = delete;
        FlightEditorModule &operator=(const FlightEditorModule &) = delete;

    private:
        class Extension;

        std::unique_ptr<Extension> mExtension;
        Registration mRegistration;
    };
} // namespace mir
