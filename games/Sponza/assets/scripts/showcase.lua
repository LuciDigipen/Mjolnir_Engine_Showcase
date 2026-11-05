local Showcase = {}

-- Set true to replay the original uninterrupted 108-second tour.
local AUTO_PLAY = false

-- Presentation stops; diagnostic samples and intermediate camera keys still
-- run along the original timeline, without requiring extra clicks.
local stops = {0, 7, 17, 28, 33, 37, 40, 46, 54, 63, 65, 69, 74, 79, 82, 95, 98, 108}

local function shot(time, eye, target, fov)
    return { time = time, eye = eye, target = target, fov = fov }
end

-- Coordinates are in the Sponza scene's world space. Repeated shots hold the
-- framing while the audience watches a change in the same view.
local shots = {
    shot(0,   {0.38, 2.10, 9.30}, {0.0, 1.80, -2.00}, 60),
    shot(5,   {0.10, 2.10, 7.65}, {0.0, 1.30,  0.40}, 58),
    shot(13,  {0.10, 2.10, 7.65}, {0.0, 1.30,  0.40}, 58),
    shot(17,  {-1.12, 1.35, 5.92}, {-1.42, 0.91, 3.23}, 53),
    shot(21,  {-1.12, 1.35, 5.92}, {-1.42, 0.91, 3.23}, 53),
    shot(25,  {-0.38, 1.47, 5.65}, {-1.42, 0.91, 3.23}, 53),
    shot(28,  {0.18, 1.65, 3.88}, {-1.39, 1.17, 0.85}, 39),
    shot(31,  {0.18, 1.65, 3.88}, {-1.39, 1.17, 0.85}, 39),
    shot(33,  {0.0, 3.30, 5.60}, {0.0, 2.90, -2.80}, 84),
    shot(37,  {0.0, 3.30, 5.60}, {0.0, 2.90, -2.80}, 84),
    shot(39,  {0.24, 2.35, 5.76}, {0.65, 1.25,  2.60}, 56),
    shot(50,  {0.24, 2.35, 5.76}, {0.65, 1.25,  2.60}, 56),
    shot(53,  {0.02, 2.55, 2.40}, {0.08, 0.50, -0.52}, 52),
    shot(59,  {0.02, 2.55, 2.40}, {0.08, 0.50, -0.52}, 52),
    shot(63,  {0.15, 1.79, -1.64}, {-0.18, 1.03, -4.23}, 53),
    shot(78,  {0.15, 1.79, -1.64}, {-0.18, 1.03, -4.23}, 53),
    shot(82,  {0.08, 3.60, -4.19}, {-0.06, 0.50, -7.13}, 58),
    shot(91,  {0.08, 3.60, -4.19}, {-0.06, 0.50, -7.13}, 58),
    shot(94,  {-0.28, 1.42, -6.82}, {0.08, 0.88, -9.20}, 48),
    shot(101, {-0.28, 1.42, -6.82}, {0.08, 0.88, -9.20}, 48),
    shot(104, {-0.55, 2.55, -8.20}, {1.15, 2.10, -8.40}, 56),
    shot(108, {0.0, 2.83, -8.00}, {0.10, 2.00, 5.30}, 66),
}

local function smooth(t)
    return t * t * t * (t * (t * 6 - 15) + 10)
end

local function blend(a, b, t)
    return a + (b - a) * t
end

local function vector(x, y, z)
    return {x = x, y = y, z = z}
end

local function mixPoint(a, b, t)
    return vector(blend(a[1], b[1], t), blend(a[2], b[2], t), blend(a[3], b[3], t))
end

local function lookAt(eye, target)
    local dx, dy, dz = target.x - eye.x, target.y - eye.y, target.z - eye.z
    local yaw = 0.5 * math.atan2(-dx, -dz)
    local pitch = 0.5 * math.atan2(dy, math.sqrt(dx * dx + dz * dz))
    local sy, cy = math.sin(yaw), math.cos(yaw)
    local sp, cp = math.sin(pitch), math.cos(pitch)
    return {x = cy * sp, y = sy * cp, z = -sy * sp, w = cy * cp}
end

local function updateCamera(self, world, camera)
    while self.shot > 1 and self.time < shots[self.shot].time do
        self.shot = self.shot - 1
    end
    while self.shot < #shots - 1 and self.time >= shots[self.shot + 1].time do
        self.shot = self.shot + 1
    end
    local a, b = shots[self.shot], shots[self.shot + 1]
    local t = smooth(math.min(1, (self.time - a.time) / (b.time - a.time)))
    local eye = mixPoint(a.eye, b.eye, t)
    local target = mixPoint(a.target, b.target, t)
    world:set(camera, "Transform", "position", eye)
    world:set(camera, "Transform", "rotation", lookAt(eye, target))
    world:set(camera, "Camera", "fovYDegrees", blend(a.fov, b.fov, t))
end

local function isHold(a, b)
    if a.fov ~= b.fov then return false end
    for axis = 1, 3 do
        if a.eye[axis] ~= b.eye[axis] or a.target[axis] ~= b.target[axis] then return false end
    end
    return true
end

local function advanceCamera(self, target, dt)
    -- Narration is now click-paced. Skip stationary spans, spending real time
    -- only on the original moving segments, in either direction.
    local direction = target > self.time and 1 or -1
    while self.time ~= target do
        local segment = 1
        while segment < #shots - 1 and self.time >= shots[segment + 1].time do
            segment = segment + 1
        end
        if direction < 0 and self.time == shots[segment].time then
            segment = math.max(1, segment - 1)
        end
        local a, b = shots[segment], shots[segment + 1]
        local limit = direction > 0 and math.min(target, b.time) or math.max(target, a.time)
        if isHold(a, b) then
            self.time = limit
        elseif dt > 0 then
            local step = math.min(math.abs(limit - self.time), dt)
            self.time = self.time + direction * step
            dt = dt - step
        else
            break
        end
    end
end

local function setLight(world, entry, fraction)
    world:set(entry.entity, "Light", "intensity", entry.intensity * fraction)
    -- The raster path still assigns a shadow map to a zero-intensity point
    -- or spot. Only the lighting exhibit needs those local shadow casters.
    local lightType = fraction > 0 and entry.castsShadows and entry.lightType or 3
    world:set(entry.entity, "Light", "type", lightType)
end

local function release(world, entities)
    for _, entity in ipairs(entities) do
        assert(world:set(entity, "RigidBody", "motion", 1), "could not release showcase rigid body")
    end
end

local function playFoxClip(world, fox, clip)
    -- ModelPlayer resolves and caches an asset ID even while paused. Clear it
    -- so the newly selected clip index is used on the next animation update.
    world:set(fox, "ModelPlayer", "animationAsset", "")
    world:set(fox, "ModelPlayer", "clip", clip)
    world:set(fox, "ModelPlayer", "time", 0)
    world:set(fox, "ModelPlayer", "playing", true)
end

local function startMotion(self, world)
    self.motionTime = 0
    -- Real scene entities, kept in shot together on the same platform.
    local count = 0
    for row = 0, 7 do
        for column = 0, 12 do
            local x, z = (column - 6) * 0.19 - 0.06, -6.01 - row * 0.32
            local entity = world:spawn()
            local size = 0.13
            local tint = row % 2 == 0 and {x = 0.22, y = 0.67, z = 0.81, w = 1}
                or {x = 0.95, y = 0.56, z = 0.21, w = 1}
            world:add(entity, "Transform", {position = vector(x, 0.50, z), scale = vector(size, size, size)})
            world:add(entity, "WorldTransform")
            world:add(entity, "MeshRenderer", {meshName = "builtin:cube", materialShader = "default", tint = tint})
            count = count + 1
            self.movers[count] = {entity = entity, x = x, z = z, phase = column * 0.47 + row * 0.59}
        end
    end
    print("Sponza showcase: spawned " .. count .. " moving entities")
end

local function updateMotion(self, world)
    for _, item in ipairs(self.movers) do
        local y = 0.50 + 0.14 * math.sin(self.motionTime * 2.6 + item.phase)
        world:set(item.entity, "Transform", "position", vector(item.x, y, item.z))
    end
end

-- Fixed camera and identical post settings for the two fountain samples.
-- These are frame-loop timings, separate from a hardware profiler capture.
local timingWindows = {
    {63.2, 64.9, "fountain off"},
    {67.1, 68.8, "fountain on"},
    {83.0, 90.5, "104 moving entities"},
}

local function sampleFrameTime(self, dt)
    local window = timingWindows[self.timingWindow]
    if not window or self.time < window[1] then return end
    if self.time < window[2] then
        self.frameTimes[#self.frameTimes + 1] = dt
        return
    end

    local samples = self.frameTimes
    if #samples > 0 then
        local total = 0
        for _, sample in ipairs(samples) do total = total + sample end
        table.sort(samples)
        local p95 = samples[math.ceil(#samples * 0.95)]
        print(string.format("Sponza timing (%s): %d samples, %.2f ms mean, %.2f ms p95, %.2f ms worst",
            window[3], #samples, 1000 * total / #samples, 1000 * p95, 1000 * samples[#samples]))
    end
    self.frameTimes = {}
    self.timingWindow = self.timingWindow + 1
end

local events = {
    {0, "Directional light"},
    {7, "Local lights", function(self, world)
        for _, entry in ipairs(self.localLights) do setLight(world, entry, 1) end
    end},
    {17, "Primitives and material roughness"},
    {28, "Alpha-blended GGX and 2D texture"},
    {33, "Directional sun sweep", function(self, world)
        self.sunSweepTime = 0
        for _, entry in ipairs(self.localLights) do setLight(world, entry, 0) end
    end},
    {37, "Restore the exhibit lighting", function(self, world)
        self.sunSweepTime = nil
        world:set(self.sun, "Transform", "rotation", self.sunPose)
        world:set(self.sun, "Light", "color", self.sunColor)
        world:set(self.sky, "Sky", "sunIntensity", self.skyIntensity)
        for _, entry in ipairs(self.localLights) do setLight(world, entry, 1) end
    end},
    {40, "Rigid bodies: equal-height sphere drop", function(self, world)
        release(world, self.drop)
    end},
    {41.04, "Sample sphere rebound", function(self, world)
        print(string.format("Sponza physics: amber y %.2f m, blue y %.2f m",
            world:world_position(self.amber).y, world:world_position(self.blue).y))
    end},
    {46, "Rigid bodies: off-center capsule impact", function(self, world)
        release(world, self.striker)
    end},
    {49, "Sample the toppled stack", function(self, world)
        print(string.format("Sponza physics: upper tower block y %.2f m",
            world:world_position(self.towerTop).y))
    end},
    {54, "Rigid bodies: low vs high friction", function(self, world)
        release(world, self.sliders)
    end},
    {57, "Sample slider positions", function(self, world)
        print(string.format("Sponza physics: low-friction z %.2f m, high-friction z %.2f m",
            world:world_position(self.lowFriction).z, world:world_position(self.highFriction).z))
    end},
    {60, "Light the far gallery", function(self, world)
        for _, entry in ipairs(self.localLights) do
            if entry.lightType == 2 then
                setLight(world, entry, 0)
            elseif entry.castsShadows then
                world:set(entry.entity, "Light", "type", 3)
            end
        end
    end},
    {65, "GPU particle fountain", function(self, world)
        assert(world:particle_system_start(self.fountain), "could not start particle fountain")
    end},
    {69, "Bloom on", function(_, world)
        world:set_bloom({enabled = true, threshold = 0.85, intensity = 0.37})
    end},
    {74, "Depth of field: near to far", function(self, world)
        self.focusTime = 0
        world:set_depth_of_field({enabled = true, maxBlur = 18})
    end},
    {79, "Restore post-processing", function(self, world)
        self.focusTime = nil
        world:particle_system_stop(self.fountain)
        world:set_bloom(self.bloom)
        world:set_depth_of_field(self.dof)
    end},
    {82, "Script-driven motion field", startMotion},
    {91, "Model animation: Walk", function(self, world)
        self.motionFinished = true
        playFoxClip(world, self.fox, 1)
    end},
    {95, "Sample the Walk clip", function(self, world)
        print("Sponza Fox Walk asset: " .. world:get(self.fox, "ModelPlayer").animationAsset)
    end},
    {98, "Model animation: Run", function(self, world)
        playFoxClip(world, self.fox, 2)
    end},
    {100, "Sample the Run clip", function(self, world)
        print("Sponza Fox Run asset: " .. world:get(self.fox, "ModelPlayer").animationAsset)
    end},
    {108, "Tour complete"},
}

function Showcase.NewInstanceData()
    return {}
end

function Showcase.OnStart(self, world, camera)
    self.time, self.frame, self.shot, self.event = 0, 0, 1, 1
    self.ready, self.stopIndex = false, 1
    self.motionTime, self.sunSweepTime, self.focusTime = nil, nil, nil
    self.motionFinished = false
    self.timingWindow, self.frameTimes = 1, {}
    self.localLights, self.striker, self.movers = {}, {}, {}
    self.bloom, self.dof = world:get_bloom(), world:get_depth_of_field()
    world:set_bloom({enabled = false})
    world:set_depth_of_field({enabled = false})

    world:each("Name", function(entity, name)
        local label = name.name
        if label == "Sky" then
            self.sky = entity
            self.skyIntensity = world:get(entity, "Sky").sunIntensity
        elseif label == "Sun" then
            self.sun = entity
            self.sunPose = world:get(entity, "Transform").rotation
            self.sunColor = world:get(entity, "Light").color
        elseif label == "Showcase Fountain" then
            self.fountain = entity
        elseif label == "Fox | Authored glTF animation" then
            self.fox = entity
        elseif label == "Amber Sphere | Restitution 0.80" then
            self.amber = entity
        elseif label == "Blue Sphere | Restitution 0.05" then
            self.blue = entity
        elseif label == "Coral Capsule | Off-Center Impact" then
            self.striker[1] = entity
        elseif label == "Tower Block 3" then
            self.towerTop = entity
        elseif label == "Low Friction Slider" then
            self.lowFriction = entity
        elseif label == "High Friction Slider" then
            self.highFriction = entity
        elseif world:has(entity, "Light") then
            local light = world:get(entity, "Light")
            local entry = {entity = entity, intensity = light.intensity, lightType = light.type,
                castsShadows = label == "Spot_Centre" or label == "Warm Key | Point Light"}
            self.localLights[#self.localLights + 1] = entry
            setLight(world, entry, 0)
        end
    end)

    assert(self.sky and self.sun and self.fountain and self.fox and self.amber and self.blue
        and self.lowFriction and self.highFriction and self.towerTop and #self.striker == 1,
        "Sponza showcase is missing a named exhibit")
    self.drop = {self.amber, self.blue}
    self.sliders = {self.lowFriction, self.highFriction}
    self.ready = true
    updateCamera(self, world, camera)
    if not AUTO_PLAY then
        print("Sponza presentation: left click = next cue, right click = previous camera position; events run once")
    end
end

function Showcase.OnUpdate(self, world, camera, dt)
    if not self.ready then return end
    if AUTO_PLAY then
        self.time = math.min(stops[#stops], self.time + dt)
    else
        if world:just_pressed("showcase_previous") then
            self.stopIndex = math.max(1, self.stopIndex - 1)
        elseif world:just_pressed("showcase_next") then
            self.stopIndex = math.min(#stops, self.stopIndex + 1)
        end
        advanceCamera(self, stops[self.stopIndex], dt)
    end
    self.frame = self.frame + 1
    updateCamera(self, world, camera)
    -- Waiting for a click pauses the camera only, never simulation. Frame
    -- timing windows retain their original meaning only in the automatic tour.
    if AUTO_PLAY then sampleFrameTime(self, dt) end

    while self.event <= #events and self.time >= events[self.event][1] do
        local event = events[self.event]
        print(string.format("Sponza showcase [%.2fs, frame %d]: %s", self.time, self.frame, event[2]))
        self.event = self.event + 1
        if event[3] then event[3](self, world) end
    end

    if self.motionTime and not self.motionFinished then
        self.motionTime = self.motionTime + dt
        updateMotion(self, world)
    end

    -- These clocks start once at their cue. Rewinding the camera does not
    -- rewind the sun or focus pull, and holding still lets each effect finish.
    if self.sunSweepTime then
        self.sunSweepTime = math.min(4, self.sunSweepTime + dt)
        local t = smooth(self.sunSweepTime / 4)
        -- Sweep across the open roof, moving the shadow boundary across the floor.
        local tilt = blend(-0.64, 0.64, t)
        local s, c = math.sin(tilt * 0.5), math.cos(tilt * 0.5)
        local sun = self.sunPose
        world:set(self.sun, "Transform", "rotation", {
            x = c * sun.x - s * sun.y, y = c * sun.y + s * sun.x,
            z = c * sun.z + s * sun.w, w = c * sun.w - s * sun.z,
        })
        local noon = 1 - math.abs(tilt) / 0.64
        world:set(self.sun, "Light", "color", vector(1, blend(0.60, 0.96, noon), blend(0.33, 0.88, noon)))
        world:set(self.sky, "Sky", "sunIntensity", self.skyIntensity * blend(0.72, 1.55, noon))
    end

    if self.focusTime then
        self.focusTime = math.min(4, self.focusTime + dt)
        local t = smooth(self.focusTime / 4)
        world:set(camera, "Camera", "focusDistance", blend(1.5, 4.1, t))
    end
end

function Showcase.OnStop(self, world)
    if self.bloom then world:set_bloom(self.bloom) end
    if self.dof then world:set_depth_of_field(self.dof) end
end

return Showcase
