-- Measurement body: compares each settled contact point with the baked terrain surface.
local Probe = {}

local kSettleSeconds = 1.0
local kReportSeconds = 45.0

function Probe.NewInstanceData()
    return { t = 0.0, contactAt = nil, n = 0, sum = 0.0, worst = 0.0, reported = false, x = 0.0, y = 0.0, z = 0.0, h = 0.0 }
end

function Probe.OnUpdate(self, world, entity, dt)
    if self.reported then
        return
    end
    self.t = self.t + dt
    for _, hit in ipairs(world:collisions(entity)) do
        if hit.other ~= nil and world:has(hit.other, "TerrainInstance") then
            self.contactAt = self.contactAt or self.t
            if self.t - self.contactAt >= kSettleSeconds then
                local p = hit.point
                local r = world:terrain_height_at({ x = p.x, y = p.y, z = p.z }, 1.0)
                if r.hasHeight and r.worldHeight ~= nil then
                    local diff = p.y - r.worldHeight
                    self.n = self.n + 1
                    self.sum = self.sum + math.abs(diff)
                    self.worst = math.max(self.worst, math.abs(diff))
                    self.x, self.y, self.z, self.h = p.x, p.y, p.z, r.worldHeight
                end
            end
        end
    end
    if self.t >= kReportSeconds then
        self.reported = true
        local position = world:get(entity, "Transform").position
        print(string.format("collision_probe %s contactAt=%s samples=%d mean=%.4f worst=%.4f point=%.3f,%.3f,%.3f height=%.4f bodyY=%.3f",
            world:get(entity, "Name").name, tostring(self.contactAt), self.n, self.n > 0 and self.sum / self.n or 0.0,
            self.worst, self.x, self.y, self.z, self.h, position.y))
    end
end

return Probe
