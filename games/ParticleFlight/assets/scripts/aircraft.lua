-- @file aircraft.lua
-- @author sumin.park
-- @brief Scripted aircraft attachment path for moving GPU exhaust.
-- @copyright Copyright (c) 2026 DigiPen (USA) Corporation
local Aircraft = {}
function Aircraft.NewInstanceData()
    return { seconds = 0 }
end
function Aircraft.OnUpdate(self, world, entity, dt)
    self.seconds = self.seconds + dt
    local angle = self.seconds * 0.25
    world:set(entity, "Transform", "position", {x = 96 * math.sin(angle), y = 2 + 2 * math.sin(angle * 2), z = 96 * (math.cos(angle) - 1)})
    local halfYaw = (angle - math.pi * 0.5) * 0.5
    world:set(entity, "Transform", "rotation", {x = 0, y = math.sin(halfYaw), z = 0, w = math.cos(halfYaw)})
end
return Aircraft
