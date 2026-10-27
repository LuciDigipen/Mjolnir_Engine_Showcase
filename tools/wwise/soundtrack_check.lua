-- Authors SoundtrackCheck's Wwise project through WAAPI. Run by
-- build_soundtrack_check.ps1 on a blank project, with the track's WAV in
-- SOUNDTRACK_CHECK_WAV and the SFX loop's in SOUNDTRACK_CHECK_SFX_WAV (Windows
-- PowerShell drops a "--" before script arguments).
--
-- What the engine relies on:
--   Music, SFX   busses under Main Audio Bus
--   Music_Volume, SFX_Volume
--                the game parameters audio::SetMusicVolume and SetSfxVolume write,
--                in dB from -96 (muted) to 0, each driving its bus's volume
--   Play_Soundtrack in the Music bank, a streamed, looping playlist on Music
--   Play_Sfx_Loop   in the Sfx bank, a looping sound on SFX

local function env(name)
    local value = os.getenv(name)
    assert(value ~= nil and value ~= "", name .. " must name a WAV")
    return value
end

local wav = env("SOUNDTRACK_CHECK_WAV")
local sfxWav = env("SOUNDTRACK_CHECK_SFX_WAV")

local mainBus = "\\Busses\\Default Work Unit\\Main Audio Bus"
local parameters = "\\Game Parameters\\Default Work Unit"
local playlist = "\\Containers\\Default Work Unit\\Soundtrack"
local segment = playlist .. "\\Soundtrack_Loop"
local sfxSound = "\\Containers\\Default Work Unit\\Sfx_Loop"
local vorbisLow = "\\Conversion Settings\\Factory Conversion Settings\\Vorbis\\Vorbis Quality Low"

local function volumeParameter(name)
    return { type = "GameParameter", name = name, ["@Min"] = -96, ["@Max"] = 0, ["@InitialValue"] = 0 }
end

local function volumeBus(name, parameter)
    return {
        type = "Bus",
        name = name,
        ["@RTPC"] = {
            {
                type = "RTPC",
                name = "",
                ["@PropertyName"] = "BusVolume",
                ["@ControlInput"] = parameters .. "\\" .. parameter,
                -- 1:1 in dB, except the bottom step, which is silence rather than -96 dB
                ["@Curve"] = {
                    type = "Curve",
                    points = {
                        { x = -96, y = -200, shape = "Linear" },
                        { x = -95, y = -95, shape = "Linear" },
                        { x = 0, y = 0, shape = "Linear" },
                    },
                },
            },
        },
    }
end

local function playEvent(name, target)
    return {
        type = "Event",
        name = name,
        children = { { type = "Action", name = "", ["@ActionType"] = 1, ["@Target"] = target } },
    }
end

wa_call("ak.wwise.core.object.set", {
    onNameConflict = "merge",
    listMode = "replaceAll",
    objects = {
        {
            object = parameters,
            children = { volumeParameter("Music_Volume"), volumeParameter("SFX_Volume") },
        },
        {
            object = mainBus,
            children = { volumeBus("Music", "Music_Volume"), volumeBus("SFX", "SFX_Volume") },
        },
        {
            object = "\\Containers\\Default Work Unit",
            children = {
                {
                    type = "MusicPlaylistContainer",
                    name = "Soundtrack",
                    ["@OverrideOutput"] = true,
                    ["@OutputBus"] = mainBus .. "\\Music",
                    ["@OverrideConversion"] = true,
                    ["@Conversion"] = vorbisLow,
                    ["@PlaylistRoot"] = {
                        type = "MusicPlaylistItem",
                        name = "",
                        ["@LoopCount"] = 0,
                        children = {
                            { type = "MusicPlaylistItem", name = "", ["@PlaylistItemType"] = 1, ["@Segment"] = segment },
                        },
                    },
                    children = {
                        {
                            type = "MusicSegment",
                            name = "Soundtrack_Loop",
                            ["@Tempo"] = 84,
                            ["@TimeSignatureUpper"] = 4,
                            ["@TimeSignatureLower"] = 4,
                            children = {
                                {
                                    type = "MusicTrack",
                                    name = "Soundtrack_Track",
                                    ["@IsStreamingEnabled"] = true,
                                    import = { files = { { audioFile = wav } } },
                                },
                            },
                        },
                    },
                },
                {
                    type = "Sound",
                    name = "Sfx_Loop",
                    ["@OverrideOutput"] = true,
                    ["@OutputBus"] = mainBus .. "\\SFX",
                    ["@OverrideConversion"] = true,
                    ["@Conversion"] = vorbisLow,
                    ["@IsLoopingEnabled"] = true,
                    ["@IsLoopingInfinite"] = true,
                    import = { files = { { audioFile = sfxWav } } },
                },
            },
        },
        {
            object = "\\Events\\Default Work Unit",
            children = { playEvent("Play_Soundtrack", playlist), playEvent("Play_Sfx_Loop", sfxSound) },
        },
        {
            object = "\\SoundBanks\\Default Work Unit",
            children = { { type = "SoundBank", name = "Music" }, { type = "SoundBank", name = "Sfx" } },
        },
    },
}, {})

local function include(bank, event)
    wa_call("ak.wwise.core.soundbank.setInclusions", {
        soundbank = "\\SoundBanks\\Default Work Unit\\" .. bank,
        operation = "add",
        inclusions = { { object = "\\Events\\Default Work Unit\\" .. event, filter = { "events", "structures", "media" } } },
    }, {})
    print("authored " .. event .. " in " .. bank)
end

include("Music", "Play_Soundtrack")
include("Sfx", "Play_Sfx_Loop")

wa_call("ak.wwise.core.project.save", {}, {})

-- WwiseConsole exits with what the script returns, and anything but 0 is a failure
return 0
