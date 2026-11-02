param([Parameter(Mandatory=$true)][string]$Repository,
      [Parameter(Mandatory=$true)][string]$BuildDirectory,
      [Parameter(Mandatory=$true)][string]$Publisher,
      [Parameter(Mandatory=$true)][string]$Compiler,
      [Parameter(Mandatory=$true)][string]$CxxCompiler,
      [Parameter(Mandatory=$true)][string]$MakeProgram)
$ErrorActionPreference='Stop'
$testRoot=Join-Path ([IO.Path]::GetTempPath()) ('mjolnir-publish-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$source=Join-Path $testRoot 'probe.slang'
$binary=Join-Path $testRoot 'build'
$utf8=[Text.UTF8Encoding]::new($false)
function Write-Source([string]$text){[IO.File]::WriteAllText($source,$text,$utf8)}
function Build-Publish {
    & cmake --build $binary --target publication_fixture_publish_shaders --parallel 1
    if($LASTEXITCODE -ne 0){throw "Publication target failed: $LASTEXITCODE"}
}
function Read-Pointer { return (Get-Content (Join-Path $binary 'shaders/current.json') -Raw|ConvertFrom-Json).generation }
function Output-Hash {
    $id=Read-Pointer
    $bytes=[IO.File]::ReadAllBytes((Join-Path $binary ('shaders/generations/'+$id.ToString('D16')+'/probe.probeMain.spv')))
    $hash=[Security.Cryptography.SHA256]::Create()
    try {return [BitConverter]::ToString($hash.ComputeHash($bytes))}
    finally {$hash.Dispose()}
}
$owned=$null
try {
    Write-Source 'body-A'
    & cmake -S (Join-Path $Repository 'tools/tests/ShaderPublicationFixture') -B $binary -G Ninja '-DCMAKE_BUILD_TYPE=Debug' "-DMIR_REPO_ROOT=$Repository" "-DMIR_SHADER_PUBLISHER=$Publisher" "-DSLANGC_EXECUTABLE=$Compiler" "-DFIXTURE_SOURCE=$source" "-DCMAKE_CXX_COMPILER=$CxxCompiler" "-DCMAKE_MAKE_PROGRAM=$MakeProgram"
    if($LASTEXITCODE -ne 0){throw 'Fixture configure failed'}
    Build-Publish
    $first=Read-Pointer
    $firstHash=Output-Hash
    $exe=Join-Path $binary 'publication_fixture.exe'
    if(Test-Path -LiteralPath $exe){throw 'Shader-only target unexpectedly linked the engine consumer'}
    $stamp=(Get-Item -LiteralPath $source).LastWriteTimeUtc
    Write-Source 'body-B'
    [IO.File]::SetLastWriteTimeUtc($source,$stamp)
    Build-Publish
    $second=Read-Pointer
    if($second -le $first -or (Output-Hash) -eq $firstHash){throw 'Same-size same-time content edit was missed'}
    Build-Publish
    if((Read-Pointer) -ne $second){throw 'No-op build changed generation'}
    Write-Source 'body-fail'
    & cmake --build $binary --target publication_fixture_publish_shaders --parallel 1
    if($LASTEXITCODE -eq 0 -or (Read-Pointer) -ne $second){throw 'Partial failure replaced the last-good generation'}
    Write-Source 'body-C'
    [IO.File]::WriteAllText(($source+'.hold'),'hold',$utf8)
    if(Test-Path -LiteralPath ($source+'.started')){Remove-Item -LiteralPath ($source+'.started')}
    $manifest=Join-Path $binary 'shader_build/shader_manifest.json'
    $catalog=Join-Path $binary 'shader_build/surface_materials.json'
    $runtime=Join-Path $binary 'shaders'
    $cache=Join-Path $testRoot 'late-cache'
    $arguments='--manifest "'+$manifest+'" --catalog "'+$catalog+'" --cache "'+$cache+'" --root "'+$runtime+'"'
    $owned=Start-Process -FilePath $Publisher -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $testRoot 'late.log') -RedirectStandardError (Join-Path $testRoot 'late.stderr.log')
    [void]$owned.Handle
    $deadline=[DateTime]::UtcNow.AddSeconds(10)
    while(-not(Test-Path -LiteralPath ($source+'.started'))){if([DateTime]::UtcNow -gt $deadline){throw 'Late compiler barrier not reached'};Start-Sleep -Milliseconds 20}
    Write-Source 'body-D'
    Remove-Item -LiteralPath ($source+'.hold')
    Build-Publish
    $newHash=Output-Hash
    if(-not $owned.WaitForExit(20000)){throw 'Late publisher did not finish'}
    if($owned.ExitCode -ne 0){throw 'Superseded publisher did not retry its content snapshot'}
    if((Output-Hash) -ne $newHash){throw 'Late compiler rolled back newer content'}
    $owned=$null
    Write-Output 'PASS real Ninja staging, content cache, shader-only publication, failure rollback and late compiler supersession'
}
finally {
    if(Test-Path -LiteralPath ($source+'.hold')){Remove-Item -LiteralPath ($source+'.hold')}
    if($owned -and -not $owned.HasExited){Stop-Process -Id $owned.Id}
}
