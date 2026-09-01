[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Probe
)

$ErrorActionPreference = 'Stop'
$probePath = (Resolve-Path -LiteralPath $Probe).Path
$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("srs-vhd-matrix-" + [guid]::NewGuid().ToString('N'))
$resolvedTemp = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$resolvedRoot = [System.IO.Path]::GetFullPath($testRoot)
if (-not $resolvedRoot.StartsWith($resolvedTemp, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing unexpected test directory: $resolvedRoot"
}
New-Item -ItemType Directory -Path $resolvedRoot | Out-Null
$mounted = [System.Collections.Generic.List[string]]::new()
$accessPaths = [System.Collections.Generic.List[object]]::new()

function New-TestVolume {
    param([string]$Name, [string]$FileSystem)

    $vhdPath = Join-Path $resolvedRoot "$Name.vhdx"
    New-VHD -Path $vhdPath -Dynamic -SizeBytes 1GB | Out-Null
    $disk = Mount-VHD -Path $vhdPath -Passthru | Get-Disk
    $mounted.Add($vhdPath)
    Initialize-Disk -Number $disk.Number -PartitionStyle GPT | Out-Null
    $partition = New-Partition -DiskNumber $disk.Number -UseMaximumSize -AssignDriveLetter
    Format-Volume -Partition $partition -FileSystem $FileSystem -NewFileSystemLabel "SRS-$Name" -Confirm:$false | Out-Null
    return "$($partition.DriveLetter):\"
}

try {
    $ntfs = New-TestVolume -Name 'ntfs' -FileSystem 'NTFS'
    $exfat = New-TestVolume -Name 'exfat' -FileSystem 'exFAT'
    New-Item -ItemType Directory -Path (Join-Path $ntfs 'game') | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $exfat 'game') | Out-Null

    $ntfsMode = ((& $probePath (Join-Path $ntfs 'game') |
        Select-String '^mode=').Line -replace '^mode=', '')
    if ($ntfsMode -notin @('persistent_only', 'persistent_with_warning')) {
        throw "A detachable VHDX was classified unsafely: $ntfsMode"
    }

    $ntfsPartition = Get-Partition -DriveLetter $ntfs.Substring(0, 1)
    $mountPath = Join-Path $resolvedRoot 'ntfs-volume-mount'
    New-Item -ItemType Directory -Path $mountPath | Out-Null
    $mountAccessPath = $mountPath.TrimEnd('\') + '\'
    Add-PartitionAccessPath -DiskNumber $ntfsPartition.DiskNumber `
        -PartitionNumber $ntfsPartition.PartitionNumber -AccessPath $mountAccessPath
    $accessPaths.Add([pscustomobject]@{
        DiskNumber = $ntfsPartition.DiskNumber
        PartitionNumber = $ntfsPartition.PartitionNumber
        AccessPath = $mountAccessPath
    })
    $mountedGame = Join-Path $mountPath 'mounted-game'
    New-Item -ItemType Directory -Path $mountedGame | Out-Null
    $mountedMode = ((& $probePath $mountedGame |
        Select-String '^mode=').Line -replace '^mode=', '')
    if ($mountedMode -notin @('persistent_only', 'persistent_with_warning')) {
        throw "A verified NTFS volume mount point was rejected: $mountedMode"
    }
    $driveIdentity = (& $probePath (Join-Path $ntfs 'mounted-game') |
        Select-String '^installation=').Line
    $mountIdentity = (& $probePath $mountedGame |
        Select-String '^installation=').Line
    if ($driveIdentity -ne $mountIdentity) {
        throw 'Installation identity changed through the NTFS volume mount point'
    }

    & $probePath (Join-Path $exfat 'game') persistent_only
    if ($LASTEXITCODE -ne 0) { throw 'exFAT VHD classification failed' }

    $before = (& $probePath (Join-Path $exfat 'game') persistent_only | Select-String '^installation=').Line
    Dismount-VHD -Path $mounted[1]
    $mounted.RemoveAt(1)
    $disk = Mount-VHD -Path (Join-Path $resolvedRoot 'exfat.vhdx') -Passthru | Get-Disk
    $mounted.Add((Join-Path $resolvedRoot 'exfat.vhdx'))
    $partition = Get-Partition -DiskNumber $disk.Number | Where-Object DriveLetter | Select-Object -First 1
    $usedLetters = @(Get-Volume | Where-Object DriveLetter | ForEach-Object { [string]$_.DriveLetter })
    $newLetter = @('Z','Y','X','W','V','U','T','S','R','Q','P') |
        Where-Object { $_ -notin $usedLetters } |
        Select-Object -First 1
    if (-not $newLetter) { throw 'No free drive letter is available for the identity test' }
    Set-Partition -DiskNumber $disk.Number -PartitionNumber $partition.PartitionNumber `
        -NewDriveLetter $newLetter
    $partition = Get-Partition -DiskNumber $disk.Number -PartitionNumber $partition.PartitionNumber
    $afterRoot = "$($partition.DriveLetter):\"
    $after = (& $probePath (Join-Path $afterRoot 'game') persistent_only | Select-String '^installation=').Line
    if ($before -ne $after) { throw 'Installation identity changed after VHD remount' }
}
finally {
    foreach ($accessPath in @($accessPaths)) {
        Remove-PartitionAccessPath -DiskNumber $accessPath.DiskNumber `
            -PartitionNumber $accessPath.PartitionNumber `
            -AccessPath $accessPath.AccessPath -ErrorAction SilentlyContinue
    }
    foreach ($vhdPath in @($mounted)) {
        Dismount-VHD -Path $vhdPath -ErrorAction SilentlyContinue
    }
    $checkedRoot = [System.IO.Path]::GetFullPath($resolvedRoot)
    if ($checkedRoot.StartsWith($resolvedTemp, [System.StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $checkedRoot).StartsWith('srs-vhd-matrix-')) {
        Remove-Item -LiteralPath $checkedRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host 'Windows VHDX storage matrix passed'
