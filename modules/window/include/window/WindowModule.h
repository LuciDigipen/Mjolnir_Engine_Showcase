/**
 * @file WindowModule.h
 * @author sumin.park
 * @brief Owns the engine's one window.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <window/Window.h>

#include <memory>

namespace mir
{
    class WindowModule
    {
    public:
        // throws SetupError when the window cannot be created
        explicit WindowModule(const WindowDesc &desc) : mWindow(Window::Create(desc)) {}

        WindowModule(const WindowModule &) = delete;
        WindowModule &operator=(const WindowModule &) = delete;

        Window &Get() { return *mWindow; }
        const Window &Get() const { return *mWindow; }

    private:
        std::unique_ptr<Window> mWindow;
    };
}
