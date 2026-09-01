[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RuntimeTransactionProbe,
    [Parameter(Mandatory = $true)]
    [string]$StorageBackendProbe,
    [Parameter(Mandatory = $true)]
    [string]$BaselineGameRoot,
    [Parameter(Mandatory = $true)]
    [string]$PatchRoot
)

$ErrorActionPreference = 'Stop'
$runtimeProbe = (Resolve-Path -LiteralPath $RuntimeTransactionProbe).Path
$storageProbe = (Resolve-Path -LiteralPath $StorageBackendProbe).Path
$baseline = (Resolve-Path -LiteralPath $BaselineGameRoot).Path
$patches = (Resolve-Path -LiteralPath $PatchRoot).Path
$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("srs-vhd-faults-" + [guid]::NewGuid().ToString('N'))
$resolvedTemp = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$resolvedRoot = [System.IO.Path]::GetFullPath($testRoot)
if (-not $resolvedRoot.StartsWith($resolvedTemp, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing unexpected test directory: $resolvedRoot"
}
New-Item -ItemType Directory -Path $resolvedRoot | Out-Null
$mounted = [System.Collections.Generic.List[string]]::new()
$vaults = [System.Collections.Generic.List[string]]::new()
$baselineBytes = (Get-ChildItem -LiteralPath $baseline -File -Recurse -Force |
    Measure-Object -Property Length -Sum).Sum
if (-not $baselineBytes) { throw 'The baseline fixture contains no files' }
$vhdSize = [Math]::Max(2GB, [Math]::Ceiling(($baselineBytes * 4 + 1GB) / 1MB) * 1MB)

function Get-FreeDriveLetter {
    $used = @(Get-Volume | Where-Object DriveLetter | ForEach-Object { [string]$_.DriveLetter })
    return @('Z','Y','X','W','V','U','T','S','R','Q','P','O','N','M') |
        Where-Object { $_ -notin $used } |
        Select-Object -First 1
}

function Mount-TestVhd {
    param([string]$Path, [string]$NewLetter)
    $disk = Mount-VHD -Path $Path -Passthru | Get-Disk
    $mounted.Add($Path)
    $partition = Get-Partition -DiskNumber $disk.Number |
        Where-Object Type -ne 'Reserved' |
        Select-Object -First 1
    if ($NewLetter) {
        Set-Partition -DiskNumber $disk.Number -PartitionNumber $partition.PartitionNumber `
            -NewDriveLetter $NewLetter
        $partition = Get-Partition -DiskNumber $disk.Number `
            -PartitionNumber $partition.PartitionNumber
    }
    return "$($partition.DriveLetter):\"
}

function Dismount-TestVhd {
    param([string]$Path)
    Dismount-VHD -Path $Path
    [void]$mounted.Remove($Path)
}

function Invoke-ExpectedCrash {
    param([string]$GameRoot, [int]$Phase)
    $env:SKYRIM_RUNTIME_SWAPPER_FAULT_AFTER_PHASE = [string]$Phase
    & $runtimeProbe $GameRoot $patches *> $null
    if ($LASTEXITCODE -ne -536870911) {
        throw "Fault phase $Phase returned $LASTEXITCODE instead of the injected crash code"
    }
}

function Assert-Baseline {
    param([string]$GameRoot)
    Get-ChildItem -LiteralPath $baseline -File -Recurse -Force | ForEach-Object {
        $relative = [System.IO.Path]::GetRelativePath($baseline, $_.FullName)
        $actual = Join-Path $GameRoot $relative
        if (-not (Test-Path -LiteralPath $actual -PathType Leaf)) {
            throw "Recovery omitted baseline file: $relative"
        }
        if ((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $actual -Algorithm SHA256).Hash) {
            throw "Recovery changed baseline file: $relative"
        }
    }
}

try {
    foreach ($phase in 1..10) {
        $vhdPath = Join-Path $resolvedRoot "phase-$phase.vhdx"
        New-VHD -Path $vhdPath -Dynamic -SizeBytes $vhdSize | Out-Null
        $disk = Mount-VHD -Path $vhdPath -Passthru | Get-Disk
        $mounted.Add($vhdPath)
        Initialize-Disk -Number $disk.Number -PartitionStyle GPT | Out-Null
        $partition = New-Partition -DiskNumber $disk.Number -UseMaximumSize -AssignDriveLetter
        Format-Volume -Partition $partition -FileSystem NTFS `
            -NewFileSystemLabel "SRS-Phase-$phase" -Confirm:$false | Out-Null
        $root = "$($partition.DriveLetter):\"
        $gameRoot = Join-Path $root 'game'
        New-Item -ItemType Directory -Path $gameRoot | Out-Null
        Get-ChildItem -LiteralPath $baseline -Force |
            Copy-Item -Destination $gameRoot -Recurse -Force

        $probeOutput = & $storageProbe $gameRoot --prepare
        if ($LASTEXITCODE -ne 0) { throw "VHD classification failed for phase $phase" }
        $mode = (($probeOutput | Select-String '^mode=').Line -replace '^mode=', '')
        if ($mode -notin @('persistent_only', 'persistent_with_warning')) {
            throw "A detachable VHDX was classified unsafely at phase $phase`: $mode"
        }
        $vaultLine = ($probeOutput | Select-String '^vault=').Line
        if (-not $vaultLine) { throw "Vault resolution failed for phase $phase" }
        $vaults.Add($vaultLine.Substring('vault='.Length))

        if ($phase -ge 8) {
            Invoke-ExpectedCrash -GameRoot $gameRoot -Phase 3
        }
        Invoke-ExpectedCrash -GameRoot $gameRoot -Phase $phase
        Remove-Item Env:SKYRIM_RUNTIME_SWAPPER_FAULT_AFTER_PHASE -ErrorAction SilentlyContinue

        Dismount-TestVhd -Path $vhdPath
        $newLetter = Get-FreeDriveLetter
        if (-not $newLetter) { throw 'No free drive letter is available for remount recovery' }
        $remountedRoot = Mount-TestVhd -Path $vhdPath -NewLetter $newLetter
        $remountedGame = Join-Path $remountedRoot 'game'
        & $runtimeProbe $remountedGame $patches *> $null
        if ($LASTEXITCODE -ne 0) {
            throw "Recovery failed after VHD detach at phase $phase"
        }
        Assert-Baseline -GameRoot $remountedGame
        Dismount-TestVhd -Path $vhdPath
    }
}
finally {
    Remove-Item Env:SKYRIM_RUNTIME_SWAPPER_FAULT_AFTER_PHASE -ErrorAction SilentlyContinue
    foreach ($vhdPath in @($mounted)) {
        Dismount-VHD -Path $vhdPath -ErrorAction SilentlyContinue
    }
    foreach ($vault in $vaults) {
        if ($vault -and (Split-Path -Leaf $vault).StartsWith('skyrimse-') -and
            $vault.Contains('Skyrim Runtime Swapper')) {
            Remove-Item -LiteralPath $vault -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    $checkedRoot = [System.IO.Path]::GetFullPath($resolvedRoot)
    if ($checkedRoot.StartsWith($resolvedTemp, [System.StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $checkedRoot).StartsWith('srs-vhd-faults-')) {
        Remove-Item -LiteralPath $checkedRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host 'Windows VHDX detach and recovery matrix passed'
