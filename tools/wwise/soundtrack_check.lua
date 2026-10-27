-- Authors SoundtrackCheck's Wwise project through WAAPI. Run by
-- build_soundtrack_check.ps1 on a blank project, with the track's WAV in
-- SOUNDTRACK_CHECK_WAV (Windows PowerShell drops a "--" before script arguments).
--
-- What the engine relies on:
--   Music        a bus under Main Audio Bus that the soundtrack is routed to
--   Music_Volume the game parameter audio::SetMusicVolume writes, in dB from
--                -96 (muted) to 0, driving the Music bus's volume
--   Play_Soundtrack in the Music bank, a streamed, looping playlist

local wav = os.getenv("SOUNDTRACK_CHECK_WAV")
assert(wav ~= nil and wav ~= "", "SOUNDTRACK_CHECK_WAV must name the track's WAV")

local mainBus = "\\Busses\\Default Work Unit\\Main Audio Bus"
local musicBus = mainBus .. "\\Music"
local volume = "\\Game Parameters\\Default Work Unit\\Music_Volume"
local playlist = "\\Containers\\Default Work Unit\\Soundtrack"
local segment = playlist .. "\\Soundtrack_Loop"
local event = "\\Events\\Default Work Unit\\Play_Soundtrack"
local bank = "\\SoundBanks\\Default Work Unit\\Music"

wa_call("ak.wwise.core.object.set", {
    onNameConflict = "merge",
    listMode = "replaceAll",
    objects = {
        {
            object = "\\Game Parameters\\Default Work Unit",
            children = {
                { type = "GameParameter", name = "Music_Volume", ["@Min"] = -96, ["@Max"] = 0, ["@InitialValue"] = 0 },
            },
        },
        {
            object = mainBus,
            children = {
                {
                    type = "Bus",
                    name = "Music",
                    ["@RTPC"] = {
                        {
                            type = "RTPC",
                            name = "",
                            ["@PropertyName"] = "BusVolume",
                            ["@ControlInput"] = volume,
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
                },
            },
        },
        {
            object = "\\Containers\\Default Work Unit",
            children = {
                {
                    type = "MusicPlaylistContainer",
                    name = "Soundtrack",
                    ["@OverrideOutput"] = true,
                    ["@OutputBus"] = musicBus,
                    ["@OverrideConversion"] = true,
                    ["@Conversion"] = "\\Conversion Settings\\Factory Conversion Settings\\Vorbis\\Vorbis Quality Low",
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
            },
        },
        {
            object = "\\Events\\Default Work Unit",
            children = {
                {
                    type = "Event",
                    name = "Play_Soundtrack",
                    children = {
                        { type = "Action", name = "", ["@ActionType"] = 1, ["@Target"] = playlist },
                    },
                },
            },
        },
        {
            object = "\\SoundBanks\\Default Work Unit",
            children = {
                { type = "SoundBank", name = "Music" },
            },
        },
    },
}, {})

wa_call("ak.wwise.core.soundbank.setInclusions", {
    soundbank = bank,
    operation = "add",
    inclusions = { { object = event, filter = { "events", "structures", "media" } } },
}, {})

wa_call("ak.wwise.core.project.save", {}, {})
print("authored " .. event .. " in " .. bank)

-- WwiseConsole exits with what the script returns, and anything but 0 is a failure
return 0
