/**
 * @file NgxIdentity.h
 * @author Rahul Nair
 * @brief The application identity NGX sees. Extension discovery and support
 *        detection (Device) and initialization (DlssRayReconstruction) must
 *        all present the same one. MIR_WITH_DLSS builds only.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/DlssConfig.h>

#include <core/fs/Paths.h>

#include <nvsdk_ngx_vk.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace mir::vk
{
    inline constexpr NVSDK_NGX_EngineType kNgxEngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    inline constexpr const char *kNgxEngineVersion = "1.0";

    // NGX writes its logs here, so they sit beside engine.log instead of in whatever directory the engine was launched from.
    inline const wchar_t *NgxApplicationDataPath()
    {
        static const std::wstring path = []
        {
            std::error_code error;
            std::filesystem::create_directories(LogDir(), error);
            return LogDir().wstring();
        }();
        return path.c_str();
    }
}
