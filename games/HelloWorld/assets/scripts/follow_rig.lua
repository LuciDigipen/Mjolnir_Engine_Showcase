-- Test rig for CameraFollow: drives this entity along a figure-8 so a
-- following camera has something to chase.
local FollowRig = {}

local radius = 2.5
local height = 1.0
local speed = 0.8 -- radians per second along the curve

function FollowRig.NewInstanceData()
    return { t = 0.0 }
end

function FollowRig.OnUpdate(self, world, entity, dt)
    self.t = self.t + speed * dt
    local s = math.sin(self.t)
    world:set(entity, "Transform", "position", { x = radius * s, y = height, z = radius * s * math.cos(self.t) })
end

return FollowRig
