-- This script and its spawned satellite belong to the additive beacon instance.
local SceneBeacon = {}

function SceneBeacon.NewInstanceData()
    return { time = 0.0 }
end

function SceneBeacon.OnStart(self, world, entity)
    self.origin = world:get(entity, "Transform").position
    self.instance = world:scene_instance()
    self.satellite = world:spawn() -- Inherits this script's scene ownership.
    world:add(self.satellite, "Transform", {
        position = { x = self.origin.x + 1.6, y = self.origin.y, z = self.origin.z },
        scale = { x = 0.25, y = 0.25, z = 0.25 }
    })
    world:add(self.satellite, "WorldTransform", {})
    world:add(self.satellite, "Name", { name = "Additive-owned satellite" })
    world:add(self.satellite, "MeshRenderer", {
        meshName = "builtin:cube", materialShader = "default",
        tint = { x = 1.0, y = 0.25, z = 0.6, w = 1.0 }
    })
    local demo = world:session_data().sceneDemo
    if demo then
        demo.additiveVisits = demo.additiveVisits + 1
        print("Beacon scene: visit " .. demo.additiveVisits .. ", satellite owner = " .. world:entity_scene(self.satellite))
    end
end

function SceneBeacon.OnUpdate(self, world, entity, dt)
    self.time = self.time + dt
    world:set(entity, "Transform", "position", {
        x = self.origin.x, y = self.origin.y + 0.25 * math.sin(self.time * 2.0), z = self.origin.z
    })
    if world:is_alive(self.satellite) then
        world:set(self.satellite, "Transform", "position", {
            x = self.origin.x + 1.6 * math.cos(self.time),
            y = self.origin.y + 0.4,
            z = self.origin.z + 1.6 * math.sin(self.time)
        })
    end
end

function SceneBeacon.OnStop(self, world, entity)
    -- The engine removes the satellite with this scene, no manual destroy is needed.
    print("Beacon scene: unloading instance " .. self.instance .. " and its runtime spawn.")
end

return SceneBeacon
