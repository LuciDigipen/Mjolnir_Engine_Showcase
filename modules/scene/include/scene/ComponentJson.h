/**
 * @file ComponentJson.h
 * @author Sumin Park
 * @brief Field <-> Json conversion for type-erased component fields.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "core/ecs/ComponentFields.h"

#include <nlohmann/json.hpp>

namespace mir
{
    nlohmann::json FieldToJson(FieldKind kind, const void *bytes);

    // EntityRef is resolved by the caller: nothing is written and true is returned.
    // Any other kind returns false and leaves outBytes untouched when the value does not fit.
    bool JsonToField(FieldKind kind, const nlohmann::json &value, void *outBytes);
}
