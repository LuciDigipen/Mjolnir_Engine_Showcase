# audio

Owns the Wwise sound engine's lifetime and its per-frame pump, and keeps the AK
SDK out of the rest of the engine. 

`mir::audio` · depends `mir::core`, `Wwise`, `mir::serial` (private) · API `modules/audio/include/audio/` · maintainer Dong Ta · reviewed 2026-10-27

## Model

Audiokinetic ships Wwise through its own launcher rather than vcpkg, so this is
the one dependency in the engine that is not a `find_package(... CONFIG)` away.
`cmake/FindWwise.cmake` resolves an SDK root from `WWISE_SDK_ROOT` or the
`WWISESDK` environment variable, picks the arch and toolset tree, and exposes
everything as one `Wwise::Wwise` imported target. Nothing outside that module
names a `.lib`.

The sound engine comes up in four stages that must happen in order — memory
manager, stream manager, low-level I/O, sound engine — and go down in exactly the
reverse. That ordering is the whole reason this module exists as a unit rather
than as calls scattered through `app`.

Banks arrive as build artifacts, and they belong to a game rather than to the
engine. The Wwise authoring project does not live in this repository; generated
`.bnk` files are committed to `games/<name>/audio/SoundBanks/`, ride the game
directory copy next to the executable, and the runtime resolves them from
`GameProject::mSoundBankDir`, so a moved build folder still runs, matching how
`assets`, `fonts` and `branding` already behave.

`game.json`'s `soundBanksRoot` names that directory and defaults to
`audio/SoundBanks`. It sits outside `assetsRoot`, so `AssetCooker` never walks
it — and the cooker skips `.bnk` and `.wem` outright, and a `.json` beside a
`.bnk` of the same name, so a bank dropped under a cook root by mistake is passed
over rather than buried in a blob it cannot be loaded from.

A bank's events are listed from the metadata Wwise writes beside it. The sound
engine cannot name what a bank holds, since a bank stores events by ID only, so
`ListBankEvents` reads `<bank>.json`, which Wwise generates next to `<bank>.bnk`
when the project's JSON metadata setting is on, as it is by default. It needs no
sound engine and lists every bank but `Init`; a bank without its JSON, such as
`HelloWorld`'s `Test`, is listed with no events and `mListed` false. Copy the JSON
with the bank, as `tools/wwise/build_soundtrack_check.ps1` does.

Today this is a harness, not an audio system. One hardcoded listener and one
hardcoded emitter exist so that playback can be proven end to end; there is no
positioning, no entity mapping and no component.

`AudioModule` starts Wwise with the game's SoundBank directory in its
constructor and terminates it in its destructor. It registers the audio
components, `AudioEmitterSystem`, `AudioListenerSystem`, `SoundtrackSystem` and a
render system in `LateUpdate`, in that order, and the `AudioEmitter` and
`Soundtrack` OnRemove hooks. The render system runs `audio::Update` every frame,
ungated, so UI sounds and fades still play in Edit and while paused.

Banks load in the background. `Init` loads `Init.bnk` itself and queues every
other bank with Wwise's asynchronous `LoadBank`, so startup no longer grows with
bank size. Wwise calls back on its bank thread, which only queues the result;
`Update` applies it, so a bank's state only changes on the main thread, at the
start of a frame.

Music has a path of its own. A scene names its soundtrack with a `Soundtrack`
component: a Wwise event, the bank it lives in, and a fade-out.
`SoundtrackSystem` asks for the bank while the scene is being edited, calls
`PlayMusic` on Play, and cuts the music on Stop; Pause keeps it playing. The
cut runs in the `PlayEnd` stage, before the restore removes Play's entities,
because their `OnRemove` fades: the fade-out is for a `Soundtrack` removed
during Play. `PlayMusic` holds the post until that bank has loaded, then posts on a
game object of its own. The authoring project routes the music to a `Music` bus,
and the volume and mute reach it through one game parameter.

```json
{ "type": "Soundtrack", "fields": { "eventName": "Play_Soundtrack", "bankName": "Music", "fadeOutMs": 1000 } }
```

The soundtrack's media is streamed. A track marked Stream in Wwise is written
next to the banks as `Media/<id>.wem` rather than into its bank, and the
low-level I/O reads it from there in pieces while it plays. The pieces come out
of the stream manager's I/O pool, which `Init` reserves once (2 MB by default),
so a playing track costs the same memory whatever its length.

Music and SFX each have a volume, linear from 0 to 1, and a mute, set through
`AudioSystem.h` or from Lua. Each writes its own game parameter, which the
authoring project maps onto its own bus, so neither moves the other. When both
are silent, muted or at 0, the engine waits out the ramp and suspends Wwise, so
its audio thread stops mixing. Unmuting either wakes it, and what was playing
carries on from where it stopped.

The values outlive the run. `AudioModule` reads them from
`%LOCALAPPDATA%/Mjolnir/<game>/settings.json` before Wwise starts, and writes them
back there once they change, through `serial`. The file is per user, so it is
never `project_settings.json`, which is the game's and goes into source control.

```json
{ "audio": { "music": { "volume": 0.3, "muted": true }, "sfx": { "volume": 0.7, "muted": false } } }
```

Until that file exists, the game's own defaults apply. `AudioModule` first reads
an `audio` section of the same shape from `project_settings.json`, keeps it as
`Defaults()`, and starts from it wherever the per-user file is missing or holds a
bad value. Without that section the defaults are full volume, unmuted. The
per-user file is only written once a value changes, so a player who never touches
a volume keeps getting the project's.

## API

| Name | Header | Role |
|---|---|---|
| `Init` | `audio/AudioSystem.h` | Brings up all four Wwise stages, loads `Init.bnk` and queues every other bank in a game's SoundBank directory; throws `SetupError` on any failure before that |
| `Update` | `audio/AudioSystem.h` | Applies finished bank loads, posts a soundtrack whose bank is in, then one `RenderAudio()`; nothing is mixed without it |
| `RequestBank`, `AreBanksLoading` | `audio/AudioSystem.h` | Queues a bank by name, once, and answers its `BankState`; whether any bank is still loading |
| `PlayMusic`, `StopMusic`, `IsMusicPlaying` | `audio/AudioSystem.h` | The one soundtrack: posted once its bank has loaded, stopped with a fade |
| `SetMusicVolume`, `SetMusicMuted` | `audio/AudioSystem.h` | The Music bus's volume, linear from 0 to 1, and its mute; kept while audio is down |
| `SetSfxVolume`, `SetSfxMuted` | `audio/AudioSystem.h` | The same for the SFX bus |
| `IsIdle` | `audio/AudioSystem.h` | Whether both channels are silent, so Wwise is suspended and posts are refused |
| `LoadAudioSettings`, `SaveAudioSettings` | `audio/AudioSettings.h` | A file's `audio` section, per-user or `project_settings.json`, read over the defaults given and written keeping the other sections |
| `AudioModule::Defaults` | `audio/AudioModule.h` | The `audio` section of `project_settings.json` as read at startup, which a missing per-user file falls back to |
| `ListBankEvents` | `audio/BankEvents.h` | Each bank in a SoundBank directory, but `Init`, with the event names from the `<bank>.json` beside it |
| `Soundtrack` | `audio/components/Soundtrack.h` | A scene's music: `eventName`, `bankName`, `fadeOutMs` |
| `SoundtrackSystem` | `audio/SoundtrackSystem.h` | Requests the bank, starts the music on Play and cuts it on Stop |
| `Terminate` | `audio/AudioSystem.h` | Reverse-order teardown, safe whether or not `Init` succeeded |
| `PostEvent` | `audio/AudioSystem.h` | Fires an event by name on the fixed global game object |
| `SetRTPCValue` | `audio/AudioSystem.h` | Writes a game parameter on one object, or globally with a `gameObject` of 0 |
| `SetSwitch` | `audio/AudioSystem.h` | Writes a switch on one object; Wwise has no global switch to fall back on |
| `SetState` | `audio/AudioSystem.h` | Writes a state group, which Wwise scopes globally - no object takes part |
| `WwiseSdkVersion` | `audio/Wwise.h` | The SDK version this build compiled against |
| `IsSoundEngineInitialized` | `audio/Wwise.h` | Safe before init, where it returns false |

`audio/Wwise.h` exists from the build-wiring work that preceded any runtime code.
It is the smallest thing that proves the headers compile and the import libraries
link, and it is worth keeping for exactly that.

## Rules

**No AK type appears in a public header.** The surface is `const char*` and
`bool`. Wwise's headers need `UNICODE` defined and two sample directories on the
include path, and a leaked `AkGameObjectID` would push both onto every consumer
of this module.

**A newly registered game object has no listener.** Register the listener, call
`SetDefaultListeners`, and only then register the emitter — the default set
applies to *subsequent* registrations, so an emitter registered first keeps an
empty listener set. Get this backwards and `PostEvent` still returns a valid
playing ID while nothing is audible, with no error logged anywhere.

**`Terminate()` runs on every `Init()` failure path, before the `SetupError` is thrown, not just at shutdown.** Each
stage guards on its own module's state rather than on a single "did init finish"
flag, so a partially-initialised engine tears down correctly. Debug Wwise asserts
at `MemoryMgr::Term` if anything is still allocated, which is what makes a clean
Debug exit evidence rather than luck.

**`Init` loads `Init.bnk` and queues every other `.bnk` directly in the
SoundBank directory.** `Init.bnk` carries the bus layout every other bank
resolves against, so it is the one bank `Init` waits for; it is a few hundred
bytes. A directory that holds banks but no `Init.bnk` is rejected before any
Wwise stage comes up, so the error names the missing file instead of a failed
`LoadBank`.

**An event posted before its bank has loaded does not play.** Wwise looks the
event up when it is posted and drops it when it is not there yet; nothing retries
it. So whatever posts waits: `AudioEmitterSystem` starts no `playOnStart`
emitter while any bank is still loading, since it does not know which bank holds
the event, and `PlayMusic` waits for the bank its `Soundtrack` names, or for
every bank when none is named. A post that fails anyway while banks are loading
is a warning that says so, not the usual error. This matters most in a game build,
where Play starts on the first frame.

**A bank that fails to load in the background is reported once, as an error, and
the engine keeps running.** `Init` has returned by then, so there is nothing to
throw out of. The bank is marked Failed and never queued again, and a soundtrack
waiting on it gives up with one warning. The smoke tests fail on the error, so
broken banks still stop CI.

**A bank is known by its Wwise ID, not its spelling.** Wwise hashes a name
lowercased, so `Music` and `music` are one bank; keyed by the string, the second
spelling would queue a second load, get `AK_BankAlreadyLoaded`, and be marked
Failed while the bank sat loaded. `AK_BankAlreadyLoaded` counts as loaded for the
same reason.

**One `Soundtrack` plays, and it keeps playing.** `SoundtrackSystem` remembers
the entity whose music it started and stays with it while that entity has a
`Soundtrack`, whatever order the query visits them in; another one only takes
over once that one is gone. Picking the first in query order instead could start
a second one, and removing the first would then stop the second's music for
good.

**The volumes are a contract with the authoring project.** No Wwise call sets
one bus's volume, so the engine writes the global game parameters `Music_Volume`
and `SFX_Volume`, in dB from -96 to 0, over 100 ms so the change does not click.
A volume becomes `60·log10(volume)` dB, the cube of the amplitude, so 0.5 is
-18 dB and 0.1 is -60 dB and each step sounds about as big as the last; the plain
`20·log10` crowded nine of ten steps into the top 20 dB, where they barely differ.
The project must define them and drive the `Music` and `SFX` busses' Bus Volume
from them, with -96 mapped to silence; muting writes -96 and keeps the volume to
come back to. A project without them takes the writes and nothing changes, as
with any misspelled game parameter, so `HelloWorld`, whose emitters go to the main
bus and whose authoring project is not here, is not affected by either volume.

**Nothing is posted while everything is silent.** Suspended, Wwise still
queues a posted event, and it would start the moment a channel came back however
stale it was, so `PostEventOn` returns 0 instead and logs nothing. Anything that
should be playing comes back on its own: `AudioEmitterSystem` holds its starts and
loop reposts while `IsIdle`, and a soundtrack waits as it waits for its bank. A
one-shot fired while silent is gone, which is what muted means.

**Suspended, `RenderAudio` runs ten times a second, not every frame.** Wwise
still needs it to take queued calls such as emitter positions off its queue, but
every call wakes the audio thread. Called every frame while suspended, it kept
that thread as busy as when playing; at ten a second the queue stays short.

**The settings file is reported once, and written at most twice a second.** A
missing file is one Info line and a broken one, or a bad value, one Warning, and
either falls back to the defaults without stopping anything. The file is parsed
here rather than through `ReadJsonFile`, whose parse error would count as a
second report and fail the smoke tests over a user's own file. A slider can
change a volume on every frame, so the writes are throttled, and whatever is left
is written at shutdown.

**Vorbis needs its decoder linked and registered.** `AkVorbisDecoder` is in
`FindWwise.cmake`'s library list and its factory header is included in
`AudioSystem.cpp`. Without either, a Vorbis source plays silence with no error.

**Streamed media is found under `Media/`.** `AkGeneratedSoundBanksResolver`
looks for a streamed file at `<SoundBank directory>/Media/<id>.wem`, which is
where Wwise generates it. Copy the folder with the banks; the asset cooker skips
`.wem`, and the game's assets copy carries subfolders.

**Do not ask Wwise for the speaker configuration on the first frames.**
`GetSpeakerConfiguration` waits for the output device to open, about 150 ms in
the first `Update`. The Wwise Profiler shows the output's channels without the
wait.

**The bank path is resolved from the executable, never the working directory.**
`GameProject::mSoundBankDir` is built from `mir::ExecutableDir()`, and `Init`
takes it as given rather than deriving a path of its own. `ctest` runs from the
build directory while a VS Code F5 launch typically runs from the repository
root, so a CWD-relative path passes the smoke tests and silently finds nothing
under the debugger.

**A game with no SoundBanks still starts Wwise.** Setup creates a missing
SoundBank directory, and an empty one loads no banks. The sound engine
still comes up, so a silent game and a game with audio take the same path, and a
Wwise failure is fatal for both.

**The profiler connection is part of setup.** In a build without
`AK_OPTIMIZED`, a failed `AK::Comm::Init` is fatal. It fails when another
process holds the profiler port.

### Errors

| Category | Raised when |
|---|---|
| System | The SoundBank directory is missing, a Wwise stage fails to start (the cause carries the `AKRESULT`), or the profiler connection fails |
| Project data | Banks are present without `Init.bnk`, or `Init.bnk` fails to load |

Any other bank that fails is logged, not thrown, by the rule above.

**`RenderAudio()` runs once per frame, after `mScheduler.Update`.** Calls queued
before it are processed that frame; anything posted after waits for the next one.
It sits outside `App::Run`'s window-size guard so audio keeps running while the
window is minimised.

**`AK_OPTIMIZED` is set by the imported target, never by hand.** It has to match
how the Wwise libraries themselves were built, and the same switch drops
`CommunicationCentral` and `ws2_32` from the Release link line. Defining it in
one place keeps the two from drifting.

**The low-level I/O is Wwise sample source compiled into this target**, not a
shipped library — `AkDefaultIOHookDeferred.cpp` plus two files from `Common/`.
2025.1 ships only the deferred hook; the blocking one that most tutorials use was
removed. Those sources call the wide Win32 APIs, which is why this target defines
`UNICODE`/`_UNICODE` while the rest of the engine does not.

**`PostEvent` returns a playing ID, not an `AKRESULT`.** It is checked against
`AK_INVALID_PLAYING_ID`; running it through the usual `Check()` helper would
compare against the wrong type of failure and report success for every bad event
name.

**A game parameter, switch or state is addressed by name, never by ID.**
`AK::SoundEngine` hashes the string at the call, so the banks need no string
table and nothing here has to carry generated IDs. The cost is that a misspelled
name is not an error anywhere: Wwise takes the write and it reaches nothing, so
`SetRTPCValue` returning true means "the call was made", not "something heard
it".

**Only the RTPC has a global fallback.** A game parameter with no per-object
value falls back to the global one, which is what `gameObject = 0` writes, and
reaching one object alone additionally needs the parameter scoped to Game Object
in the authoring project. A switch is per object with no fallback, and a state
is global with no per-object form - hence the three different signatures.

**There is no `AkMusicEngine` in 2025.1.** It was folded into `AkSoundEngine`, so
there is no separate init call to add, and older integration guides that list one
are describing a version this project does not use.

## State

*As of 2026-10-27.* `SoundtrackCheck` also plays a 2-second SFX loop on the `SFX`
bus from a second cube, with N to mute it and Right and Left to step its volume;
both channels step in tenths. Its banks now add `Sfx.bnk`, 12.8 KB with the loop
in memory, and `Init.bnk` carries both busses and both game parameters.

Measured in a Debug game build with temporary logging, now removed: while the
music and SFX play, Wwise's audio thread uses about 100–130 million cycles a
second; suspended it uses about 5 million, and Wwise renders about 11 buffers a
second instead of one per game frame. Two other threads rise while suspended, one
from about 21 to 40 million cycles a second and one from nothing to about 6
million, so the process saves less than the audio thread alone. Unmuting resumed
the soundtrack without a restart. Saving and restoring were run end to end with
a driver script: the values saved were the values loaded on relaunch, a launch
with both muted came up suspended with the soundtrack waiting, and a broken file
gave one warning and the defaults. Listening, and the same steps by hand in
SoundtrackCheck, have not been done.

`AudioSettings.tests.cpp`, in the new `modules/audio/tests/`, covers the file
without a sound engine: defaults, round trips, broken input and other sections
kept. The SFX Lua functions are tested next to the music ones.

*As of 2026-10-26.* The soundtrack path works in `SoundtrackCheck`, a game made
for it: a 3-minute stereo placeholder loop, streamed, on a `Music` bus, with M to
mute and Up and Down to step the volume from Lua. Its banks are built from
nothing by `tools/wwise/build_soundtrack_check.ps1`, which synthesizes the track
(`PlaceholderTrack.cs`), authors a scratch Wwise project through WAAPI
(`soundtrack_check.lua`) and copies the output into the game. `Init.bnk` is 757
bytes, `Music.bnk` 559 and the track a 1.17 MB Vorbis `.wem`, 2 channels at
48 kHz.

Measured in a Debug game build with temporary logging, now removed: `Init` takes
about 7 ms and returns with `Music.bnk` still loading. While the track plays,
Wwise holds 0 KB of media and its 2,050 KB stream I/O pool, and its total stays
at about 4.9 MB over the run. The longest audio `Update` in the first 200 frames
was 0.08–0.10 ms with the soundtrack and 0.06 ms without. Wwise reports a
2-channel output on the test machine. The editor's Profiler panel, the Wwise
Profiler and listening have not been checked in this pass.

`smoke.SoundtrackCheck` opens it in the editor and `smoke.SoundtrackCheck.game-mode`
plays it from the first frame, so the soundtrack is posted while its bank may
still be loading. The music Lua functions are tested in `AudioBindings.tests.cpp`
with no sound engine.

*As of 2026-09-18.* Working: the full init and teardown sequence in Debug and
Release, and both of `HelloWorld`'s banks loading, confirmed on hardware. The
Wwise Profiler connects to Debug and Profile builds.

Banks are per game. `HelloWorld` carries `Init.bnk` and `Test.bnk`; `EmptyScene`
has no banks, so its SoundBank directory is created empty and Wwise runs with
nothing loaded.

Everything about *what* plays is a placeholder — listener id 2 and emitter id 1
are registered at init and nothing posts to them. `PostEvent` has no caller
outside this module today; audible output was proven with a temporary call and
that call was not kept. No positioning, no distance attenuation, no entity
mapping.

Apart from the settings tests, coverage comes from `smoke.HelloWorld`, which
runs the engine for 120 frames and fails on any logged Error or Critical; since
every failure path in `Init` throws, a passing smoke test means the engine came up
and both banks loaded. It does not and cannot assert that sound reached the
speakers.

`SetRTPCValue`, `SetSwitch` and `SetState` are wired through to Lua (see
`docs/modules/script.md`) but nothing audible has been proven through them yet.
`HelloWorld`'s `Init.bnk` declares ten game parameters, and because banks carry
IDs rather than names, none of their names can be recovered from this repository
- whoever owns the authoring project has to supply one, or author an RTPC mapped
to volume or pitch, before `games/HelloWorld/assets/scripts/audio_rtpc.lua` can
be heard doing anything.

`HelloWorld`'s test bank is PCM; `SoundtrackCheck`'s track is Vorbis, so the
Vorbis decoder is linked. `Profile` maps to CMake's `RelWithDebInfo` and there
is no preset for it, so that branch of the config mapping has never been
exercised.

## Backlog

1. **An object and listener API**, then `AudioListener` and `AudioEmitter`
   components with a system in `mScheduler` that syncs transforms and posts
   events. Doing the wrapper first is deliberate: it is a mechanical refactor of
   code already proven, while the component design still has open questions —
   whether emitters post on demand or on a trigger, how 2D UI sounds bypass
   positioning, and what happens to a playing voice when its entity is destroyed.
2. **Per-object unregistration.** `Terminate` calls `UnregisterAllGameObj`, which is
   enough while the object set is fixed. Once entities come and go during play,
   destruction needs to unregister, or the session leaks game objects.
3. **Bank lifecycle.** Every bank still loads at startup, in the background now,
   and none unloads before Terminate. A real project wants banks per level or per
   scene; `RequestBank` is the start of that, and an unload with a reference
   count is the rest.
4. **Exercise the Profile configuration**, either through a preset or a one-off
   `RelWithDebInfo` configure. It is the only config path in `FindWwise.cmake`
   that has never been built.
5. **Settle where the authoring project lives.** It was removed from this
   repository, so `games/HelloWorld/audio/SoundBanks/` is currently a pair of
   binaries nobody can regenerate without rebuilding the Wwise project from
   scratch. `SoundtrackCheck` avoids it with a script that builds its project from
   nothing, which suits generated placeholder content and nothing a sound
   designer edits. Its own repository, shared storage, or back here once the
   content settles are all defensible; what would decide it is whether anyone
   outside the engine programmers edits audio. Per-game banks raise the stakes a
   little: each game that gains audio needs its own generation target, and its
   own `Init.bnk`.
6. **Soundtrack transitions.** A second `PlayMusic` cuts the first with a 10 ms
   fade. A crossfade, or a Wwise music switch driven by a state, would let a
   scene change its music without a gap.
7. **Wwise's own memory in the Profiler panel.** The panel shows the whole
   process; a Wwise line needs a route to the editor that does not link audio.

## Changed

*2026-10-27* — **Music and SFX volumes, an idle Wwise, and saved settings.**
Was: only the music had a volume and mute, Wwise mixed silence when it was muted,
and nothing outlived the run. Now: SFX has its own volume and mute on an `SFX`
bus, Wwise is suspended while both are silent and posts are refused then, and both
are saved per user and restored at the next launch. Because: the game's settings
menu (#305) needs both channels, a muted game should not spend CPU on audio, and
a setting that resets every launch is not a setting.

*2026-10-26* — **Banks load in the background, and a scene can have a
soundtrack.** Was: `Init` loaded every bank before returning, any bank failure
threw, and music was only an event an emitter could post. Now: `Init` waits for
`Init.bnk` alone and the rest load on Wwise's bank thread; a failed bank is
logged once; play-on-start emitters wait for loading to finish; and a
`Soundtrack` component plays a streamed track on a `Music` bus from Play to Stop,
with its volume and mute in Lua. Because: bank size went straight into startup
time, and music needs its own volume and must not sit in memory whole.

*2026-10-20* — **Every setup failure is fatal, and every bank loads.** Was: a
missing SoundBank directory ran silent, `Init` returned false on any failure,
the profiler connection was optional, and `Init` loaded `Init.bnk` and a
hard-coded `Test.bnk`. Now: the directory is created when missing, every `.bnk`
in it loads with `Init.bnk` first, and every failure throws a `SetupError`.
Because: a degraded start hid broken audio data, and the hard-coded bank name
tied the engine to one game.
