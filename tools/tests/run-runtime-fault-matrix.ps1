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
$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("srs-fault-matrix-" + [guid]::NewGuid().ToString('N'))
$resolvedTemp = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$resolvedRoot = [System.IO.Path]::GetFullPath($testRoot)
if (-not $resolvedRoot.StartsWith($resolvedTemp, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing unexpected test directory: $resolvedRoot"
}
New-Item -ItemType Directory -Path $resolvedRoot | Out-Null
$testVaults = [System.Collections.Generic.List[string]]::new()

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
        $gameRoot = Join-Path $resolvedRoot "phase-$phase"
        New-Item -ItemType Directory -Path $gameRoot | Out-Null
        Get-ChildItem -LiteralPath $baseline -Force |
            Copy-Item -Destination $gameRoot -Recurse -Force

        $vaultLine = (& $storageProbe $gameRoot automatic --prepare | Select-String '^vault=').Line
        if (-not $vaultLine) { throw "Could not resolve the test vault for phase $phase" }
        $testVaults.Add($vaultLine.Substring('vault='.Length))

        if ($phase -ge 8) {
            Invoke-ExpectedCrash -GameRoot $gameRoot -Phase 3
        }
        Invoke-ExpectedCrash -GameRoot $gameRoot -Phase $phase

        Remove-Item Env:SKYRIM_RUNTIME_SWAPPER_FAULT_AFTER_PHASE -ErrorAction SilentlyContinue
        & $runtimeProbe $gameRoot $patches
        if ($LASTEXITCODE -ne 0) {
            throw "Idempotent recovery failed after fault phase $phase"
        }
        Assert-Baseline -GameRoot $gameRoot
        & $runtimeProbe $gameRoot $patches *> $null
        if ($LASTEXITCODE -ne 0) {
            throw "Repeated recovery failed after fault phase $phase"
        }
        Assert-Baseline -GameRoot $gameRoot
    }
}
finally {
    Remove-Item Env:SKYRIM_RUNTIME_SWAPPER_FAULT_AFTER_PHASE -ErrorAction SilentlyContinue
    foreach ($vault in $testVaults) {
        if ($vault -and (Split-Path -Leaf $vault).StartsWith('skyrimse-') -and
            $vault.Contains('Skyrim Runtime Swapper')) {
            Remove-Item -LiteralPath $vault -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    $checkedRoot = [System.IO.Path]::GetFullPath($resolvedRoot)
    if ($checkedRoot.StartsWith($resolvedTemp, [System.StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $checkedRoot).StartsWith('srs-fault-matrix-')) {
        Remove-Item -LiteralPath $checkedRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host 'Runtime fault and recovery matrix passed'
