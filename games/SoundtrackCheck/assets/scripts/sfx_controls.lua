-- SoundtrackCheck's SFX controls: N mutes the SFX loop, Right and Left step its
-- volume in tenths. The cube spins the other way, its size follows the volume,
-- and it turns red while muted. With both channels muted the engine suspends Wwise.
local STEPS = 10
local SPIN = -0.6 -- radians a second

local SfxControls = {}

local function Step(volume, direction)
    local steps = math.floor(volume * STEPS + 0.5) + direction
    return math.max(0, math.min(STEPS, steps)) / STEPS
end

function SfxControls.NewInstanceData()
    return { angle = 0.0 }
end

function SfxControls.OnUpdate(self, world, entity, dt)
    if world:just_pressed("SfxMute") then
        world:set_sfx_muted(not world:sfx_muted())
    end

    if world:just_pressed("SfxVolumeUp") then
        world:set_sfx_volume(Step(world:sfx_volume(), 1))
    elseif world:just_pressed("SfxVolumeDown") then
        world:set_sfx_volume(Step(world:sfx_volume(), -1))
    end

    self.angle = self.angle + SPIN * dt
    local half = self.angle * 0.5
    local size = 0.25 + 0.75 * world:sfx_volume()
    world:set(entity, "Transform", "rotation", { w = math.cos(half), x = 0.0, y = math.sin(half), z = 0.0 })
    world:set(entity, "Transform", "scale", { x = size, y = size, z = size })

    local tint = world:sfx_muted() and { x = 0.9, y = 0.2, z = 0.2, w = 1.0 } or { x = 0.4, y = 0.8, z = 1.0, w = 1.0 }
    world:set(entity, "MeshRenderer", "tint", tint)
end

return SfxControls
