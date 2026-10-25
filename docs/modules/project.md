# project

A game directory read into plain data: `game.json`, and the check that
`project_settings.json` is a JSON object. It interprets no library's section;
each library reads its own values at startup.

`mir::project` · depends `mir::core_base`, `serial`, public · API `modules/project/include/project/` · maintainer Jackey Cen · reviewed 2026-10-24

## Model

A plain library: stateless helpers with no lifetime, so it links only
`core_base` and `serial`. `LoadGameProject` turns a game directory into a
`GameProject` of strings and paths. The one field it cannot fill is the asset id
root, which needs the normalizer `assets` and the cooker share: `assets`'
`ResolveAssetIdRoot` fills it, and `main` and `App::RequestOpenProject` call it
right after a load.

## API

| Type | Header | Role |
|---|---|---|
| `GameProject` | `project/GameProject.h` | `{mDir, mName, mTitle, mAssetsRoot, mAssetIdRoot, mRootDir, mAssetsDir, mSceneDir, mProjectSettingsPath, mSoundBankDir}` |
| `ProjectRoot`, `ReadDefaultGame`, `NormalizeGameDir`, `LoadGameProject`, `ResolveGameProject`, `EnsureGameDirectories`, `DiscoverGameProjects` | `project/GameProject.h` | Finding, reading and preparing a game directory |
| `ReadProjectSettings` | `project/ProjectSettings.h` | Fails setup on a malformed `project_settings.json` |

All but `DiscoverGameProjects` throw a `SetupError`. The caller passes the
category for a game that is not found: Usage when the name came from `--game`,
Project data when it came from `engine.json`. `DiscoverGameProjects` skips a
broken game with a logged error, so one bad `game.json` does not hide the others.

## Rules

**`mAssetIdRoot` is empty until `ResolveAssetIdRoot` runs.** A `GameProject`
from `DiscoverGameProjects` has none; the editor's project menu resolves each to
grey out a game whose `assetsRoot` is unusable.

**Development builds read the source tree.** `ProjectRoot` follows
`asset_workspace.json` back to the checkout, so the editor's import edits the
real game data instead of the copy next to the executable.

## State

*As of 2026-10-24.* Moved out of `app`. Tested through the `smoke` runs and
`editor`'s `ContentBrowserWatcher` tests.

## Backlog

None yet.

## Changed

*2026-10-24* — **Moved out of `app` into a plain library.** Was:
`app/GameProject.h` and the private `ProjectSettings.h`, with the asset id root
computed during the load through `assets`. Now: `project`, linking only
`core_base` and `serial`, with the id root filled by `assets`. Because:
`AssetImportManager` and `ContentBrowserWatcher` moved to `editor`, which `app`
depends on, so what they include had to sit below `editor`.
