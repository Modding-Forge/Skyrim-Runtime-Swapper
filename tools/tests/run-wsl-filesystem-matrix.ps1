[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Distribution,
    [Parameter(Mandatory = $true)]
    [string]$LinuxProbe,
    [string]$MatrixScript = (Join-Path $PSScriptRoot 'run-linux-filesystem-matrix.sh')
)

$ErrorActionPreference = 'Stop'
$status = & wsl.exe --status 2>&1
if ($LASTEXITCODE -ne 0) {
    throw "WSL is unavailable. Install WSL 2 and enable virtualization before running this release gate.`n$status"
}

$probePath = (Resolve-Path -LiteralPath $LinuxProbe).Path
$scriptPath = (Resolve-Path -LiteralPath $MatrixScript).Path
$linuxProbe = (& wsl.exe -d $Distribution -- wslpath -a $probePath.Replace('\', '/')).Trim()
$linuxScript = (& wsl.exe -d $Distribution -- wslpath -a $scriptPath.Replace('\', '/')).Trim()
if (-not $linuxProbe -or -not $linuxScript) {
    throw 'WSL path translation failed'
}

& wsl.exe -d $Distribution -u root -- bash $linuxScript $linuxProbe
if ($LASTEXITCODE -ne 0) { throw 'WSL filesystem matrix failed' }

& wsl.exe --shutdown
if ($LASTEXITCODE -ne 0) { throw 'wsl --shutdown failed' }

& wsl.exe -d $Distribution -u root -- bash $linuxScript $linuxProbe
if ($LASTEXITCODE -ne 0) { throw 'WSL matrix failed after a full shutdown/restart' }

Write-Host 'WSL filesystem and shutdown matrix passed'
