param(
  [Parameter(Mandatory = $true)]
  [string[]]$Path
)

$required = @(
  'Dynamic base',
  'High Entropy Virtual Addresses',
  'NX compatible',
  'Guard'
)
$dumpbin = (Get-Command dumpbin.exe -ErrorAction SilentlyContinue).Source
if (-not $dumpbin) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} `
    'Microsoft Visual Studio\Installer\vswhere.exe'
  if (Test-Path -LiteralPath $vswhere) {
    $installation = & $vswhere -latest -products * `
      -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
      -property installationPath
    $dumpbin = Get-ChildItem -LiteralPath (Join-Path $installation 'VC\Tools\MSVC') `
      -Filter dumpbin.exe -Recurse | Sort-Object FullName -Descending | `
      Select-Object -First 1 -ExpandProperty FullName
  }
}
if (-not $dumpbin) {
  throw 'dumpbin.exe was not found in the Visual Studio installation'
}
foreach ($binary in $Path) {
  $headers = (& $dumpbin /headers $binary | Out-String)
  if ($LASTEXITCODE -ne 0) {
    throw "dumpbin failed for $binary"
  }
  foreach ($flag in $required) {
    if ($headers -notmatch [regex]::Escape($flag)) {
      throw "$binary is missing PE hardening flag: $flag"
    }
  }
}
