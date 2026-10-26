-- Prints this body's contacts: begin and end each time, and the first stay, so a trigger zone can be checked.
local ContactLog = {}

function ContactLog.NewInstanceData()
    return { stays = 0 }
end

function ContactLog.OnUpdate(self, world, entity, dt)
    for _, hit in ipairs(world:collisions(entity)) do
        if hit.phase == "stay" then
            self.stays = self.stays + 1
            if self.stays == 1 then
                print("contact_log: stay started")
            end
        else
            local p = hit.point
            print(string.format("contact_log: %s at (%.2f, %.2f, %.2f)", hit.phase, p.x, p.y, p.z))
        end
    end
end

return ContactLog
