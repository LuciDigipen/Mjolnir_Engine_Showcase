/**
 * @file PostEffectRegistry.cpp
 * @author Rahul Nair
 * @brief Name -> params-UI callback, for the editor's Post-Processing panel.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "renderer/PostEffectRegistry.h"

#include <core/log/Assert.h>

namespace mir
{
    PostEffectRegistry &PostEffectRegistry::Instance()
    {
        static PostEffectRegistry registry;
        return registry;
    }

    void PostEffectRegistry::Register(std::string_view name, void (*drawParams)(nlohmann::json &))
    {
        MIR_CHECK(!name.empty(), "PostEffectRegistry::Register: effect name is empty");

        for (const PostEffectDesc &existing : mEffects)
        {
            MIR_CHECK(existing.mName != name,
                "PostEffectRegistry: \"{}\" is already registered - effect names must be unique", name);
        }

        mEffects.push_back(PostEffectDesc{ name, drawParams });
    }
}
