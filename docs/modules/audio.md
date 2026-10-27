# audio

Owns the Wwise sound engine's lifetime and its per-frame pump, and keeps the AK
SDK out of the rest of the engine. 

`mir::audio` · depends `mir::core`, `Wwise` (private) · API `modules/audio/include/audio/` · maintainer Dong Ta · reviewed 2026-10-26

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
it — and the cooker skips `.bnk` and `.wem` outright, so a bank dropped under a
cook root by mistake is passed over rather than buried in a blob it cannot be
loaded from.

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

## API

| Name | Header | Role |
|---|---|---|
| `Init` | `audio/AudioSystem.h` | Brings up all four Wwise stages, loads `Init.bnk` and queues every other bank in a game's SoundBank directory; throws `SetupError` on any failure before that |
| `Update` | `audio/AudioSystem.h` | Applies finished bank loads, posts a soundtrack whose bank is in, then one `RenderAudio()`; nothing is mixed without it |
| `RequestBank`, `AreBanksLoading` | `audio/AudioSystem.h` | Queues a bank by name, once, and answers its `BankState`; whether any bank is still loading |
| `PlayMusic`, `StopMusic`, `IsMusicPlaying` | `audio/AudioSystem.h` | The one soundtrack: posted once its bank has loaded, stopped with a fade |
| `SetMusicVolume`, `SetMusicMuted` | `audio/AudioSystem.h` | The Music bus's volume, linear from 0 to 1, and its mute; kept while audio is down |
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

**The music volume is a contract with the authoring project.** No Wwise call
sets one bus's volume, so the engine writes the global game parameter
`Music_Volume`, in dB from -96 to 0, over 100 ms so the change does not click.
The project must define it and drive the `Music` bus's Bus Volume from it, with
-96 mapped to silence; muting writes -96 and keeps the volume to come back to. A
project without it takes the writes and nothing changes, as with any misspelled
game parameter.

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

There is no `modules/audio/tests/`. Coverage comes from `smoke.HelloWorld`, which
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
