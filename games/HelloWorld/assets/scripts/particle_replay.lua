-- Replays a one-shot ParticleSystem (an explosion) every few seconds so it can be watched repeatedly.
local ParticleReplay = {}

local interval = 4.0 -- seconds between replays

function ParticleReplay.NewInstanceData()
    return { t = 0.0 }
end

function ParticleReplay.OnUpdate(self, world, entity, dt)
    self.t = self.t + dt
    if self.t >= interval then
        self.t = 0.0
        world:particle_system_reset(entity)
    end
end

return ParticleReplay
