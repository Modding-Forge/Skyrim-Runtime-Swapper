[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Probe,
    [Parameter(Mandatory)][string]$SourceCore,
    [Parameter(Mandatory)][string]$SourceExe,
    [Parameter(Mandatory)][string]$OriginalBeafarmer,
    [Parameter(Mandatory)][string]$CleanBeafarmer,
    [Parameter(Mandatory)][string]$TestParent,
    [ValidateRange(1,100)][int]$Repetitions = 3
)

$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$probePath = (Resolve-Path -LiteralPath $Probe).Path
$patchRoot = Join-Path $repo 'assets/runtime/1.7.104-to-1.6.1170-clean'
$manifest = Get-Content -Raw -LiteralPath (Join-Path $patchRoot 'manifest.json') | ConvertFrom-Json
$parentPath = (Resolve-Path -LiteralPath $TestParent).Path
$testRoot = Join-Path $parentPath ('srs-bee-repro-' + [guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$originals = Join-Path $testRoot 'originals'
$beeRelative = 'Data/ccvsvsse004-beafarmer.esl'
$beeEntry = $manifest.files | Where-Object path -eq $beeRelative
if ((Get-FileHash -LiteralPath $CleanBeafarmer).Hash.ToLowerInvariant() -ne $beeEntry.targetSha256) {
    throw 'Clean Beafarmer does not match the release target'
}
foreach ($entry in $manifest.files) {
    $inputPath = if ($entry.path -eq $beeRelative) { $OriginalBeafarmer }
        elseif ($entry.path.StartsWith('Data/')) { Join-Path $SourceCore $entry.path }
        else { Join-Path $SourceExe $entry.path }
    if ((Get-FileHash -LiteralPath $inputPath).Hash.ToLowerInvariant() -ne $entry.sourceSha256) {
        throw "Source hash mismatch: $inputPath"
    }
    $destination = Join-Path $originals $entry.path
    New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null
    Copy-Item -LiteralPath $inputPath -Destination $destination
}

# No game launch, Defender changes, symlinks, or mutations to input files.
# Retain each unique sandbox and its journals for inspection; never delete state.
$cases = @(
    @{ Name='original'; Bee='source'; Expect='success' },
    @{ Name='absent'; Bee='absent'; Expect='success' },
    @{ Name='clean-with-manifest'; Bee='target'; Expect='manifest-control' },
    @{ Name='backups-no-manifest'; Bee='target'; Expect='backups-no-manifest' },
    @{ Name='missing-selection'; Bee='target'; Expect='missing-selection' },
    @{ Name='before-manifest'; Bee='target'; Expect='before-manifest'; Resume=$true },
    @{ Name='after-manifest'; Bee='target'; Expect='after-manifest'; Resume=$true },
    @{ Name='after-journal'; Bee='target'; Expect='after-journal'; Resume=$true }
)
for ($index=1; $index -le $Repetitions; $index++) {
    $cases += @{ Name="clean-no-manifest-$index"; Bee='target'; Expect='success' }
}
foreach ($case in $cases) {
    $game = Join-Path $testRoot "$($case.Name)/SteamLibrary/steamapps/common/Repro"
    New-Item -ItemType Directory -Path $game | Out-Null
    foreach ($entry in $manifest.files) {
        if ($entry.path -eq $beeRelative -and $case.Bee -eq 'absent') { continue }
        $source = if ($entry.path -eq $beeRelative -and $case.Bee -eq 'target') {
            $CleanBeafarmer
        } else { Join-Path $originals $entry.path }
        $destination = Join-Path $game $entry.path
        New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination
    }
    New-Item -ItemType File -Path (Join-Path $game '.srs-isolated-repro') | Out-Null
    Write-Output "CASE $($case.Name)"
    & $probePath $game $patchRoot $originals $case.Expect 2>&1 |
        Tee-Object -FilePath (Join-Path $testRoot "$($case.Name).log")
    if ($LASTEXITCODE -ne 0) { throw "Unexpected result for $($case.Name): $LASTEXITCODE; retained at $testRoot" }
    if ($case.Resume) {
        & $probePath $game $patchRoot $originals 'resume' 2>&1 |
            Tee-Object -FilePath (Join-Path $testRoot "$($case.Name)-resume.log")
        if ($LASTEXITCODE -ne 0) { throw "Resume failed for $($case.Name): $LASTEXITCODE; retained at $testRoot" }
    }
}
Write-Output "All $($cases.Count) regression cases passed (including fresh-process resumes). Fixtures retained: $testRoot"
