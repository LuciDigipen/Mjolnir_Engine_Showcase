/**
 * @file MaterialResolution.h
 * @author Jackey Cen
 * @brief Turns a caller's material description into the registered runtime path.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/Material.h>
#include <renderer/SurfaceMaterialCatalog.h>

#include <optional>
#include <string>

namespace mir
{
    // On a shared-table device, the public "default" name resolves to the
    // catalog's reflected default while preserving topology, culling, alpha,
    // sampler and an explicit record. Catalog stems resolve to their surface
    // registration; unknown surface requests fail instead of becoming legacy.
    // A device without shared tables keeps the legacy "default" shader and
    // rejects surface-only requests.
    std::optional<MaterialDesc> ResolveMaterialDesc(const MaterialDesc &requested,
        const SurfaceMaterialCatalog &catalog, bool sharedTables, std::string *error = nullptr);
}
