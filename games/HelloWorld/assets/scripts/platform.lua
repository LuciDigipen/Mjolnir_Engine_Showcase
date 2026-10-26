-- Test rig for a kinematic RigidBody: writes the Transform each physics step, and physics moves the body to
-- each target, so a crate standing on it is carried and anything in the way is pushed.
local Platform = {}

local originX = 2.4
local y = -1.2
local z = -1.4
local range = 1.2
local speed = 0.7 -- radians per second

function Platform.NewInstanceData()
    return { t = 0.0 }
end

function Platform.OnFixedUpdate(self, world, entity, dt)
    self.t = self.t + speed * dt
    world:set(entity, "Transform", "position", { x = originX + range * math.sin(self.t), y = y, z = z })
end

return Platform
