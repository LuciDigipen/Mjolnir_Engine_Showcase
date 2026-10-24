/**
 * @file SplashLinux.cpp
 * @author Rahul Nair
 * @brief No splash backend on Linux yet - Show() succeeds without showing anything.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "window/Splash.h"

#include "core/log/Assert.h"

namespace mir
{
    // Never actually created, but unique_ptr<SplashImpl>'s destructor still
    // needs the type complete wherever it's instantiated - including here.
    struct SplashImpl
    {
    };

    SplashScreen::SplashScreen() = default;
    SplashScreen::~SplashScreen() = default;

    // An unsupported platform is not a failure; Win32 throws when its splash cannot appear
    bool SplashScreen::Show(const SplashDesc &) { return true; }

    void SplashScreen::SetProgress(const char *status, float)
    {
        MIR_ASSERT(status != nullptr, "SplashScreen::SetProgress: status must not be null");
    }

    void SplashScreen::Close() {}
}
