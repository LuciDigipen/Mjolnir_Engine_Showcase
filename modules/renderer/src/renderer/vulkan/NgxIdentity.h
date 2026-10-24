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

#include <nvsdk_ngx_vk.h>

namespace mir::vk
{
    inline constexpr NVSDK_NGX_EngineType kNgxEngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    inline constexpr const char *kNgxEngineVersion = "1.0";
    inline constexpr const wchar_t *kNgxApplicationDataPath = L".";
}
