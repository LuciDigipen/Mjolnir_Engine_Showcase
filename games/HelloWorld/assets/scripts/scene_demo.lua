-- N cycles primary scenes, B loads or unloads the additive beacon display.
local SceneDemo = {}
local scenes = { "default", "demo_gallery", "demo_plaza" }
local timeout = 10.0

local function sceneIndex(world, entity)
    local name = world:get(entity, "Name")
    local id = name and string.match(name.name, "^SceneDemo/(.+)$")
    for index, candidate in ipairs(scenes) do
        if candidate == id then return index end
    end
    return 1
end

function SceneDemo.NewInstanceData()
    return { enabled = false, additiveWait = 0.0, additiveReady = false }
end

function SceneDemo.OnStart(self, world, entity)
    local data = world:session_data()
    local existing = data.sceneDemo
    if existing and world:is_alive(existing.controller) then
        -- Reloading a scene creates another saved controller, keep the persistent one.
        if world:scene_instance() == world:primary_scene() then
            existing.index = sceneIndex(world, entity)
        end
        world:destroy(entity)
        return
    end

    if not world:persist(entity) then return end
    self.enabled = true
    self.state = { controller = entity, index = sceneIndex(world, entity), swaps = 0, additiveVisits = 0 }
    data.sceneDemo = self.state
    self.primary = world:primary_scene()
    print("Scene demo: N = next scene, B = toggle additive beacons. Click the Game view first.")
    print("Scene demo: running " .. scenes[self.state.index] .. ", controller survives scene replacement.")
end

function SceneDemo.OnUpdate(self, world, entity, dt)
    if not self.enabled then return end

    local primary = world:primary_scene()
    if primary ~= self.primary then
        self.primary = primary
        local replaced = self.pending ~= nil
        if replaced then
            self.state.index = self.pending.index
            self.state.swaps = self.state.swaps + 1
        end
        local controller = world:find_scene_entity(primary, 30)
        if controller then self.state.index = sceneIndex(world, controller) end
        local label = (controller or replaced) and scenes[self.state.index] or ("instance " .. primary)
        self.pending = nil
        if replaced or (self.additiveReady and not world:scene_loaded(self.additive)) then
            self.additive = nil -- A full replacement unloads the previous additive instances too.
            self.additiveReady = false
            self.additiveWait = 0.0
        end
        print("Scene demo: " .. label .. ", completed swaps = " .. self.state.swaps)
    end

    if self.pending then
        self.pending.elapsed = self.pending.elapsed + dt
        if self.pending.elapsed >= timeout then
            print("Scene demo: replacement is still pending or failed, see the engine log.")
            self.pending = nil
        end
    end

    if self.additive then
        if world:scene_loaded(self.additive) then
            if not self.additiveReady then
                self.additiveReady = true
                local beacon = world:find_scene_entity(self.additive, 1)
                print("Scene demo: additive instance " .. self.additive .. " is ready, beacon found = " .. tostring(beacon ~= nil))
            end
        elseif self.additiveReady then
            self.additive = nil
            self.additiveReady = false
        else
            self.additiveWait = self.additiveWait + dt
            if self.additiveWait >= timeout then
                world:unload_scene(self.additive) -- Cancels an unfinished additive request too.
                self.additive = nil
                print("Scene demo: additive load timed out or failed, B retries it.")
            end
        end
    end

    if world:just_pressed("SceneDemoNext") and not self.pending then
        local nextIndex = self.state.index % #scenes + 1
        if world:load_scene(scenes[nextIndex]) then
            self.pending = { index = nextIndex, elapsed = 0.0 }
            print("Scene demo: preparing " .. scenes[nextIndex])
        end
    elseif world:just_pressed("SceneDemoAdditive") and not self.pending then
        if self.additive then
            world:unload_scene(self.additive)
            self.additive = nil
            self.additiveReady = false
            print("Scene demo: unloading beacons and their scene-owned satellite.")
        else
            self.additive = world:load_scene_additive("demo_beacons")
            self.additiveWait = 0.0
            self.additiveReady = false
            print("Scene demo: preparing additive beacons, the primary scene keeps running.")
        end
    end
end

function SceneDemo.OnStop(self, world, entity)
    if self.enabled then print("Scene demo: Play ended, session state is released.") end
end

return SceneDemo
