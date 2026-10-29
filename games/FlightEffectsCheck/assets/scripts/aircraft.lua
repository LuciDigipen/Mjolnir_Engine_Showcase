local Flight = {}

local function path(t, base)
    local accelerating = math.min(t, 15)
    local travel = 12 * t + 0.4 * accelerating * accelerating + 12 * math.max(t - 15, 0)
    local angle = travel / 96
    return {x = base.x + 96 * math.sin(angle), y = base.y + 2 * math.sin(angle * 2),
        z = base.z + 96 * (math.cos(angle) - 1)}, angle
end

function Flight.NewInstanceData()
    return {seconds = 0, stopped = false, gust = false}
end

function Flight.OnStart(self, world, entity)
    self.camera = world:has(entity, "Camera")
    self.base = world:get(entity, "Transform").position
    if self.camera then
        self.base.y = self.base.y - 6
        self.base.z = self.base.z - 22
    end
    if world:has(entity, "ParticleSystem") then
        assert(world:particle_system_wind(entity, 2, 0, -1))
    end
end

function Flight.OnUpdate(self, world, entity, dt)
    self.seconds = self.seconds + dt
    local position, angle = path(self.seconds, self.base)
    local rotation
    if self.camera then
        position.y = position.y + 6
        position.z = position.z + 22
        if self.seconds >= 60 and self.seconds < 70 then position.x = position.x + 10000 end
        local pitch = -math.atan(6 / 22) / 2
        local yaw = (self.seconds >= 60 and self.seconds < 70) and math.pi / 2 or 0
        rotation = {x = math.sin(pitch) * math.cos(yaw), y = math.cos(pitch) * math.sin(yaw),
            z = -math.sin(pitch) * math.sin(yaw), w = math.cos(pitch) * math.cos(yaw)}
    else
        local yaw = (angle - math.pi / 2) / 2
        rotation = {x = 0, y = math.sin(yaw), z = 0, w = math.cos(yaw)}
        if world:has(entity, "ParticleSystem") then
            if self.seconds >= 40 and not self.gust then
                assert(world:particle_system_wind(entity, -3, 0.5, 1))
                self.gust = true
            end
            if self.seconds >= 80 and not self.stopped then
                assert(world:particle_system_stop(entity))
                self.stopped = true
            end
        end
    end
    world:set(entity, "Transform", "position", position)
    world:set(entity, "Transform", "rotation", rotation)
end

return Flight
