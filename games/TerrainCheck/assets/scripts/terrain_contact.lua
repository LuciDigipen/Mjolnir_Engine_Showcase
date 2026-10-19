-- Placeholder: logs this body's first terrain contact, once.
local TerrainContact = {}

function TerrainContact.NewInstanceData()
    return { landed = false }
end

function TerrainContact.OnUpdate(self, world, entity, dt)
    if self.landed then
        return
    end

    for _, hit in ipairs(world:collisions(entity)) do
        if hit.phase == "begin" and hit.other ~= nil and world:has(hit.other, "TerrainInstance") then
            self.landed = true
            local p, n = hit.point, hit.normal
            print(string.format("terrain_contact: first terrain contact at (%.2f, %.2f, %.2f) normal (%.2f, %.2f, %.2f)",
                p.x, p.y, p.z, n.x, n.y, n.z))
            return
        end
    end
end

return TerrainContact
