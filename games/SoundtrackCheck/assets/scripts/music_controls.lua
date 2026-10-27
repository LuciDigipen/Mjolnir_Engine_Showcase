-- SoundtrackCheck's controls: M mutes the music, Up and Down step its volume.
-- The cube spins, its size follows the volume, and it turns red while muted.
local STEP = 0.1
local SPIN = 0.6 -- radians a second

local MusicControls = {}

function MusicControls.NewInstanceData()
    return { angle = 0.0 }
end

function MusicControls.OnUpdate(self, world, entity, dt)
    if world:just_pressed("MusicMute") then
        world:set_music_muted(not world:music_muted())
    end

    if world:just_pressed("MusicVolumeUp") then
        world:set_music_volume(math.min(1.0, world:music_volume() + STEP))
    elseif world:just_pressed("MusicVolumeDown") then
        world:set_music_volume(math.max(0.0, world:music_volume() - STEP))
    end

    self.angle = self.angle + SPIN * dt
    local half = self.angle * 0.5
    local size = 0.25 + 0.75 * world:music_volume()
    world:set(entity, "Transform", "rotation", { w = math.cos(half), x = 0.0, y = math.sin(half), z = 0.0 })
    world:set(entity, "Transform", "scale", { x = size, y = size, z = size })

    local tint = world:music_muted() and { x = 0.9, y = 0.2, z = 0.2, w = 1.0 } or { x = 1.0, y = 1.0, z = 1.0, w = 1.0 }
    world:set(entity, "MeshRenderer", "tint", tint)
end

return MusicControls
