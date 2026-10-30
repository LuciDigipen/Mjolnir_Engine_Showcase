/**
 * @file MaterialResolution.cpp
 * @author Jackey Cen
 * @brief Turns a caller's material description into the registered runtime path.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/MaterialResolution.h>

namespace mir
{
    namespace
    {
        void SetError(std::string *error, std::string message)
        {
            if (error != nullptr)
                *error = std::move(message);
        }

        const SurfaceMaterialRegistration *FindDefault(const SurfaceMaterialCatalog &catalog)
        {
            return catalog.FindByAsset("assets/materials/default.material.json");
        }
    }

    std::optional<MaterialDesc> ResolveMaterialDesc(const MaterialDesc &requested,
        const SurfaceMaterialCatalog &catalog, bool sharedTables, std::string *error)
    {
        MaterialDesc resolved = requested;
        const SurfaceMaterialRegistration *byStem = catalog.Find(requested.shaderName, requested.surfaceAsset);
        const bool wantsDefault = requested.shaderName == "default";
        const bool wantsSurface = requested.surfaceMaterial || byStem != nullptr;

        if (!sharedTables)
        {
            if (wantsSurface)
            {
                SetError(error, "surface material '" + requested.shaderName +
                                    "' requires the shared material tables");
                return std::nullopt;
            }
            if (wantsDefault)
            {
                resolved.surfaceMaterial = false;
                resolved.surfaceAsset.clear();
            }
            return resolved;
        }

        if (wantsSurface || wantsDefault)
        {
            const SurfaceMaterialRegistration *registration = nullptr;
            if (wantsDefault && requested.surfaceAsset.empty())
                registration = FindDefault(catalog);
            else if (requested.surfaceMaterial || byStem != nullptr)
                registration = catalog.Find(requested.shaderName, requested.surfaceAsset);

            if (registration == nullptr)
            {
                SetError(error, "surface material '" + requested.shaderName + "' is not registered");
                return std::nullopt;
            }

            resolved.shaderName = registration->stem;
            resolved.surfaceAsset = registration->asset;
            resolved.surfaceMaterial = true;
            return resolved;
        }

        return resolved;
    }
}
