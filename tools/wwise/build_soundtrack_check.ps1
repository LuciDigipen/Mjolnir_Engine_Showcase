<#
.SYNOPSIS
    Builds games/SoundtrackCheck's SoundBanks from nothing.

.DESCRIPTION
    Synthesizes the placeholder track (PlaceholderTrack.cs), creates a blank Wwise
    project under builds/wwise, authors it with soundtrack_check.lua, generates the
    Windows banks and copies Init.bnk, Music.bnk and the streamed media into the
    game. The Wwise project is scratch: this script is its source.

.EXAMPLE
    powershell -NoProfile -File tools/wwise/build_soundtrack_check.ps1
#>
[CmdletBinding()]
param(
    # Defaults to $env:WWISEROOT, which the Wwise Launcher sets.
    [string]$WwiseRoot = $env:WWISEROOT
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$workDir = Join-Path $repoRoot 'builds\wwise\SoundtrackCheck'
$project = Join-Path $workDir 'SoundtrackCheck\SoundtrackCheck.wproj'
$wav = Join-Path $workDir 'soundtrack.wav'
$bankDir = Join-Path $repoRoot 'games\SoundtrackCheck\assets\audio\SoundBanks'

if (-not $WwiseRoot) {
    Write-Error 'WWISEROOT is not set; pass -WwiseRoot or install Wwise through the Launcher.'
}
$console = Join-Path $WwiseRoot 'Authoring\x64\Release\bin\WwiseConsole.exe'
if (-not (Test-Path $console)) {
    Write-Error "WwiseConsole not found at $console"
}

function Invoke-WwiseConsole {
    & $console @args
    if ($LASTEXITCODE -ne 0) {
        Write-Error "WwiseConsole $($args[0]) failed with exit code $LASTEXITCODE"
    }
}

if (Test-Path $workDir) {
    Remove-Item -Recurse -Force $workDir
}
New-Item -ItemType Directory -Path $workDir | Out-Null

Add-Type -TypeDefinition (Get-Content -Raw (Join-Path $PSScriptRoot 'PlaceholderTrack.cs')) -Language CSharp
[PlaceholderTrack]::Write($wav)
Write-Host ("placeholder track: {0:N0} s" -f [PlaceholderTrack]::Seconds)

Invoke-WwiseConsole create-new-project $project --quiet
$env:SOUNDTRACK_CHECK_WAV = $wav
Invoke-WwiseConsole execute-lua-script (Join-Path $PSScriptRoot 'soundtrack_check.lua') --project $project --no-color
Invoke-WwiseConsole generate-soundbank $project --platform Windows --quiet

$generated = Join-Path (Split-Path -Parent $project) 'GeneratedSoundBanks\Windows'

# the media IDs change with every import, so stale .wem files would pile up
New-Item -ItemType Directory -Path (Join-Path $bankDir 'Media') -Force | Out-Null
Get-ChildItem (Join-Path $bankDir 'Media') -Filter *.wem | Remove-Item -Force

Copy-Item (Join-Path $generated 'Init.bnk'), (Join-Path $generated 'Music.bnk') $bankDir
Copy-Item (Join-Path $generated 'Media\*.wem') (Join-Path $bankDir 'Media')

Get-ChildItem $bankDir -Recurse -File | ForEach-Object {
    Write-Host ("{0,-24} {1,10:N0} bytes" -f $_.Name, $_.Length)
}
